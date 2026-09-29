#include "library/dao/fscueoverridestore.h"

#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QSqlError>
#include <QSqlQuery>
#include <algorithm>
#include <optional>
#include <utility>

#include "library/dao/fsstore.h"
#include "library/dao/fsstorewriter.h"
#include "preferences/systemsettings.h"
#include "track/cue.h"
#include "track/track.h"
#include "util/assert.h"

namespace {

// Per-filesystem layout, alongside the analysis cache in the same .bitedj dir.
const QString kStoreDbName = QStringLiteral("cues.sqlite");
const char kLogTag[] = "FsCueOverrideStore";

const QString kCreateTableDdl = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS cue_overrides ("
        "relpath TEXT PRIMARY KEY NOT NULL, "
        "version INTEGER NOT NULL, "
        "updated_at TEXT, "
        "cues TEXT NOT NULL)");

// Payload format version, so a future change to the stored fields can be told
// apart from the current one instead of being misread.
constexpr int kPayloadVersion = 1;

// Baseline marker for a track whose override was cleared while it stayed
// loaded. Not valid JSON, so it can never equal a serialized cue set.
const QByteArray kSuppressedBaseline = QByteArrayLiteral("\x01suppressed");

// An empty cue set. Stored as an override in its own right (the DJ deleted
// every cue), but not worth creating a first entry for.
const QByteArray kEmptyCues = QByteArrayLiteral("[]");

const QString kSlotKey = QStringLiteral("slot");
const QString kTypeKey = QStringLiteral("type");
const QString kPositionKey = QStringLiteral("pos");
const QString kEndPositionKey = QStringLiteral("end");
const QString kLabelKey = QStringLiteral("label");
const QString kColorKey = QStringLiteral("color");

/// One stored cue. `slot` is the hotcue index for both banks, or
/// Cue::kNoHotCue for the main cue. Positions are seconds from the start of
/// the file, so they survive a move to a unit that decodes differently.
struct StoredCue {
    int slot = Cue::kNoHotCue;
    int type = static_cast<int>(mixxx::CueType::HotCue);
    double startSeconds = 0.0;
    double endSeconds = -1.0;
    QString label;
    mixxx::RgbColor::code_t color = 0;
};

bool isManagedSlot(int slot) {
    return (slot >= mixxx::kHotCueBankStart &&
                   slot < mixxx::kHotCueBankStart + mixxx::kHotCueBankSize) ||
            (slot >= mixxx::kMemoryCueBankStart &&
                    slot < mixxx::kMemoryCueBankStart + mixxx::kMemoryCueBankSize);
}

/// Whether the store owns this cue, i.e. whether it is one the DJ can set from
/// the pads: both hotcue banks, plus the main cue. Everything else (intro,
/// outro, the analyzer's N60dBSound range) is left to the ordinary library.
bool isManagedCue(const CuePointer& pCue) {
    if (pCue->getType() == mixxx::CueType::MainCue) {
        return true;
    }
    if (pCue->getType() != mixxx::CueType::HotCue &&
            pCue->getType() != mixxx::CueType::Loop) {
        return false;
    }
    return isManagedSlot(pCue->getHotCue());
}

/// The cues of `track` the store owns, as they would be stored, in the order
/// the track holds them. `sampleRate` must be valid.
QList<StoredCue> storedCuesOf(const Track& track, mixxx::audio::SampleRate sampleRate) {
    QList<StoredCue> storedCues;
    const QList<CuePointer> cuePoints = track.getCuePoints();
    for (const CuePointer& pCue : cuePoints) {
        if (!isManagedCue(pCue)) {
            continue;
        }
        const mixxx::audio::FramePos startPosition = pCue->getPosition();
        if (!startPosition.isValid()) {
            continue;
        }
        StoredCue storedCue;
        storedCue.slot = pCue->getType() == mixxx::CueType::MainCue
                ? Cue::kNoHotCue
                : pCue->getHotCue();
        storedCue.type = static_cast<int>(pCue->getType());
        storedCue.startSeconds = startPosition.value() / sampleRate;
        const mixxx::audio::FramePos endPosition = pCue->getEndPosition();
        if (endPosition.isValid()) {
            storedCue.endSeconds = endPosition.value() / sampleRate;
        }
        storedCue.label = pCue->getLabel();
        storedCue.color = pCue->getColor();
        storedCues.append(storedCue);
    }
    return storedCues;
}

/// The payload for `storedCues`: a compact JSON array sorted by slot.
QByteArray serializeStoredCues(QList<StoredCue> storedCues) {
    // Sorted so that the same cue set always serializes to the same bytes,
    // which is what makes the baseline comparison meaningful. Stable, so that
    // a list parsed back from a payload and serialized again keeps the order
    // it had, which rebaselineMainCue() relies on.
    std::stable_sort(storedCues.begin(),
            storedCues.end(),
            [](const StoredCue& a, const StoredCue& b) { return a.slot < b.slot; });

    QJsonArray array;
    for (const StoredCue& storedCue : std::as_const(storedCues)) {
        QJsonObject object;
        object.insert(kSlotKey, storedCue.slot);
        object.insert(kTypeKey, storedCue.type);
        object.insert(kPositionKey, storedCue.startSeconds);
        if (storedCue.endSeconds >= 0.0) {
            object.insert(kEndPositionKey, storedCue.endSeconds);
        }
        if (!storedCue.label.isEmpty()) {
            object.insert(kLabelKey, storedCue.label);
        }
        object.insert(kColorKey, static_cast<int>(storedCue.color));
        array.append(object);
    }
    return QJsonDocument(array).toJson(QJsonDocument::Compact);
}

/// Every cue object in `payload`, in payload order. A cue without a start
/// position comes back with a negative one; what to do with it is left to the
/// caller. Returns false, with the reason in `pError`, when the payload is not
/// a JSON array at all.
bool parseStoredCues(const QByteArray& payload,
        QList<StoredCue>* pStoredCues,
        QString* pError) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        *pError = parseError.errorString();
        return false;
    }
    const QJsonArray array = document.array();
    for (const QJsonValue& value : array) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject object = value.toObject();
        StoredCue storedCue;
        storedCue.slot = object.value(kSlotKey).toInt(Cue::kNoHotCue);
        storedCue.type = object.value(kTypeKey).toInt(
                static_cast<int>(mixxx::CueType::HotCue));
        storedCue.startSeconds = object.value(kPositionKey).toDouble(-1.0);
        storedCue.endSeconds = object.value(kEndPositionKey).toDouble(-1.0);
        storedCue.label = object.value(kLabelKey).toString();
        storedCue.color = static_cast<mixxx::RgbColor::code_t>(
                object.value(kColorKey).toInt(0));
        pStoredCues->append(storedCue);
    }
    return true;
}

/// Every current-version override in the store at `target`, keyed by relative
/// path. True with nothing read when the drive has no store (the common case
/// for a stick that has never been cued here); false when a store is there but
/// could not be opened or read, which the caller must not mistake for empty.
/// An empty DDL, so nothing is ever written to the drive.
bool readStoredOverrides(const FsStoreTarget& target, QHash<QString, QByteArray>* pRows) {
    if (!QFileInfo::exists(target.dbPath)) {
        return true;
    }
    ScopedFsStore store(kLogTag);
    if (!store.open(target, QString())) {
        return false;
    }
    QSqlQuery query(store.database());
    query.setForwardOnly(true);
    query.prepare(QStringLiteral(
            "SELECT relpath, cues FROM cue_overrides WHERE version = :version"));
    query.bindValue(QStringLiteral(":version"), kPayloadVersion);
    if (!query.exec()) {
        // Also the path taken by a store written by a future schema.
        qDebug() << kLogTag << ": cannot read overrides on" << target.rootPath
                 << query.lastError().text();
        return false;
    }
    while (query.next()) {
        pRows->insert(query.value(0).toString(), query.value(1).toString().toUtf8());
    }
    return true;
}

} // anonymous namespace

QMutex FsCueOverrideStore::s_baselineMutex;
QHash<QString, QByteArray> FsCueOverrideStore::s_baselines;
QHash<QString, QByteArray> FsCueOverrideStore::s_importedCues;
QHash<QString, FsCueOverrideStore::Mirror> FsCueOverrideStore::s_mirrors;

QByteArray FsCueOverrideStore::serializeCues(const Track& track) {
    const mixxx::audio::SampleRate sampleRate = track.getSampleRate();
    VERIFY_OR_DEBUG_ASSERT(sampleRate.isValid()) {
        return kEmptyCues;
    }
    return serializeStoredCues(storedCuesOf(track, sampleRate));
}

void FsCueOverrideStore::applyPayload(Track* pTrack, const QByteArray& payload) {
    const mixxx::audio::SampleRate sampleRate = pTrack->getSampleRate();
    VERIFY_OR_DEBUG_ASSERT(sampleRate.isValid()) {
        // Stored positions are seconds; without a rate every cue would land
        // on frame 0.
        return;
    }
    QList<StoredCue> storedCues;
    QString parseError;
    if (!parseStoredCues(payload, &storedCues, &parseError)) {
        qWarning() << "FsCueOverrideStore: ignoring unreadable cue override for"
                   << pTrack->getLocation() << parseError;
        return;
    }

    QHash<int, StoredCue> cuesBySlot;
    std::optional<StoredCue> mainCue;
    for (const StoredCue& storedCue : std::as_const(storedCues)) {
        if (storedCue.startSeconds < 0.0) {
            continue;
        }
        if (storedCue.type == static_cast<int>(mixxx::CueType::MainCue)) {
            mainCue = storedCue;
        } else if (isManagedSlot(storedCue.slot)) {
            cuesBySlot.insert(storedCue.slot, storedCue);
        }
    }

    const auto framePosOf = [sampleRate](double seconds) {
        return seconds < 0.0 ? mixxx::audio::kInvalidFramePos
                             : mixxx::audio::FramePos(seconds * sampleRate);
    };
    // A pad holds a plain cue or a saved loop, and which one it is follows the
    // range rather than the stored type — the same rule the rekordbox import
    // uses, and the one that keeps a hand-edited or future-version payload
    // from putting an unusable cue type on a deck.
    const auto typeOf = [](const StoredCue& storedCue) {
        return storedCue.endSeconds >= 0.0 ? mixxx::CueType::Loop
                                           : mixxx::CueType::HotCue;
    };

    // Update the slots that survive in place and drop the ones the override
    // does not have: the track is handed straight to a deck, and one that is
    // already playing must not have its pads rebuilt underneath it.
    QList<CuePointer> staleCues;
    const QList<CuePointer> cuePoints = pTrack->getCuePoints();
    for (const CuePointer& pCue : cuePoints) {
        if (pCue->getType() != mixxx::CueType::HotCue &&
                pCue->getType() != mixxx::CueType::Loop) {
            continue;
        }
        const int slot = pCue->getHotCue();
        if (!isManagedSlot(slot)) {
            continue;
        }
        const auto it = cuesBySlot.constFind(slot);
        if (it == cuesBySlot.constEnd()) {
            staleCues.append(pCue);
            continue;
        }
        pCue->setStartAndEndPosition(
                framePosOf(it->startSeconds), framePosOf(it->endSeconds));
        pCue->setType(typeOf(*it));
        pCue->setLabel(it->label);
        pCue->setColor(mixxx::RgbColor(it->color));
        cuesBySlot.erase(it);
    }
    for (const CuePointer& pCue : std::as_const(staleCues)) {
        pTrack->removeCue(pCue);
    }

    for (auto it = cuesBySlot.constBegin(); it != cuesBySlot.constEnd(); ++it) {
        const CuePointer pCue = pTrack->createAndAddCue(
                typeOf(*it),
                it->slot,
                framePosOf(it->startSeconds),
                framePosOf(it->endSeconds),
                mixxx::RgbColor(it->color));
        pCue->setLabel(it->label);
    }

    if (mainCue) {
        pTrack->setMainCuePosition(framePosOf(mainCue->startSeconds));
        const CuePointer pMainCue = pTrack->findCueByType(mixxx::CueType::MainCue);
        if (pMainCue) {
            pMainCue->setLabel(mainCue->label);
            pMainCue->setColor(mixxx::RgbColor(mainCue->color));
        }
    }
}

void FsCueOverrideStore::applyOverrides(Track* pTrack) {
    VERIFY_OR_DEBUG_ASSERT(pTrack) {
        return;
    }
    const QString location = pTrack->getLocation();
    if (location.isEmpty() || !SystemSettings::isOnRemovableMedia(location)) {
        // A track on the internal drive keeps its cues in the library, where
        // they were already; there is no drive for them to follow.
        return;
    }
    if (!pTrack->getSampleRate().isValid()) {
        // Stored positions are seconds and cannot be placed without one. Leave
        // the track unbaselined so a later load (by then the file has been
        // decoded, so the rate is known) applies the override instead.
        return;
    }

    QByteArray payload;
    bool found = false;
    QByteArray imported;
    bool hasImported = false;
    if (readOverride(location, &payload, &found) && found) {
        // What the source library exported for this track, captured before the
        // override is laid over it: the rekordbox ANLZ cues that readAnalyze
        // has just put on, or the Serato markers the library database holds.
        // Settings → Clear puts exactly this back, so clearing takes off the
        // DJ's own edits and nothing else.
        imported = serializeCues(*pTrack);
        hasImported = true;
        applyPayload(pTrack, payload);
    }

    // Baseline what the track carries now, override applied or not, so that
    // only an edit made from here on counts as a change worth storing.
    const QByteArray baseline = serializeCues(*pTrack);
    QMutexLocker locker(&s_baselineMutex);
    s_baselines.insert(location, baseline);
    if (hasImported) {
        s_importedCues.insert(location, imported);
    }
}

// static
void FsCueOverrideStore::rebaselineMainCue(const Track& track) {
    const QString location = track.getLocation();
    const mixxx::audio::SampleRate sampleRate = track.getSampleRate();
    if (location.isEmpty() || !sampleRate.isValid() ||
            !SystemSettings::isOnRemovableMedia(location)) {
        return;
    }

    // Read off the track before the lock is taken, so this never holds the
    // track's mutex and the store's at once.
    QList<StoredCue> mainCues;
    const QList<StoredCue> trackCues = storedCuesOf(track, sampleRate);
    for (const StoredCue& storedCue : trackCues) {
        if (storedCue.type == static_cast<int>(mixxx::CueType::MainCue)) {
            mainCues.append(storedCue);
        }
    }

    QMutexLocker locker(&s_baselineMutex);
    const auto it = s_baselines.find(location);
    if (it == s_baselines.end() || *it == kSuppressedBaseline) {
        // Nothing to move: an unbaselined track is left to flushIfChanged()'s
        // own rule, and a suppressed one must stay suppressed.
        return;
    }
    QList<StoredCue> baselineCues;
    QString parseError;
    if (!parseStoredCues(*it, &baselineCues, &parseError)) {
        DEBUG_ASSERT(!"baseline is not a serialized cue set");
        return;
    }
    // The track's main cue first, then every other entry of the baseline in
    // the order it had. The stable sort in serializeStoredCues() keeps that
    // order, so those entries come out byte for byte as they went in.
    QList<StoredCue> rebased = mainCues;
    for (const StoredCue& storedCue : std::as_const(baselineCues)) {
        if (storedCue.type != static_cast<int>(mixxx::CueType::MainCue)) {
            rebased.append(storedCue);
        }
    }
    *it = serializeStoredCues(std::move(rebased));
}

void FsCueOverrideStore::flushIfChanged(const Track& track) {
    const QString location = track.getLocation();
    if (location.isEmpty() || !track.getSampleRate().isValid() ||
            !SystemSettings::isOnRemovableMedia(location)) {
        return;
    }

    const QByteArray payload = serializeCues(track);
    {
        QMutexLocker locker(&s_baselineMutex);
        const auto it = s_baselines.constFind(location);
        if (it == s_baselines.constEnd()) {
            // Never baselined by applyOverrides(). Every path that fetches or
            // adds a track calls it, so this is a track it had to skip for
            // want of a sample rate, or one built outside the library. Treat
            // the baseline as empty, so a track first cued in this session is
            // stored, but one that has no cues at all does not get an entry
            // of its own.
            if (payload == kEmptyCues) {
                return;
            }
        } else if (*it == kSuppressedBaseline || *it == payload) {
            return;
        }
    }

    FsStoreTarget target;
    if (!FsStoreTarget::resolveForFile(location, kStoreDbName, &target) ||
            !target.writable) {
        // Unavailable, or a write-protected stick: an expected case, not a
        // failure worth logging on every save. The baseline stays where it
        // was, so a later save tries again.
        return;
    }
    const QString rootKey = QDir::cleanPath(target.rootPath);
    const QString relPath = target.relPath;
    std::optional<QByteArray> previous;
    {
        QMutexLocker locker(&s_baselineMutex);
        const auto it = s_baselines.constFind(location);
        if (it != s_baselines.constEnd()) {
            // Checked again: the lock was let go while the target resolved,
            // and a clear (suppressing) or rebaselineMainCue() from the
            // analyzer thread may have moved the baseline in that window.
            if (*it == kSuppressedBaseline || *it == payload) {
                return;
            }
            previous = *it;
        }
        // Baselined now rather than once the write lands, so that a second
        // save before the writer gets to this one (the eviction save, say)
        // does not queue the same cues again.
        s_baselines.insert(location, payload);
        Mirror& mirror = mirrorForWrite(target);
        mirror.payloads.insert(relPath, payload);
        mirror.dirty.insert(relPath);
    }
    // Submitted outside the lock: with no writer the task runs right here and
    // takes the lock itself, and with one, submit() may wait out a flush that
    // is itself waiting for a task that needs this lock.
    FsStoreWriter::submit(rootKey, [location, rootKey, relPath, previous]() {
        QByteArray written;
        if (!takePendingWrite(rootKey, relPath, &written)) {
            // An earlier task already wrote this save along with its own, or
            // the drive's store was cleared or the drive ejected since.
            return;
        }
        const FsStoreWriteResult result = writeOverride(location, rootKey, written);
        if (result != FsStoreWriteResult::Failed) {
            return;
        }
        qWarning() << kLogTag << ": could not save the cues of" << location
                   << "to the drive; they stay on the track for this session and "
                      "the next save of it tries again";
        // Still shown for this session, and reported by forgetFilesystem() if
        // it never lands.
        markUnsaved(rootKey, relPath);
        // Put the baseline back, so the next save of this track retries. Only
        // if it is still the cues that failed: a newer save, or a clear, has
        // moved it on since, and that one wins.
        QMutexLocker locker(&s_baselineMutex);
        const auto it = s_baselines.constFind(location);
        if (it == s_baselines.constEnd() || *it != written) {
            return;
        }
        if (previous) {
            s_baselines.insert(location, *previous);
        } else {
            s_baselines.remove(location);
        }
    });
}

bool FsCueOverrideStore::readOverride(
        const QString& trackLocation, QByteArray* pPayload, bool* pFound) {
    *pFound = false;
    FsStoreTarget target;
    if (!FsStoreTarget::resolveForFile(trackLocation, kStoreDbName, &target)) {
        return false;
    }

    QMutexLocker locker(&s_baselineMutex);
    const Mirror* pMirror = loadedMirror(target);
    if (!pMirror) {
        // A store is there but could not be read. Not "no override": the next
        // load tries the drive again.
        return false;
    }
    const auto it = pMirror->payloads.constFind(target.relPath);
    if (it != pMirror->payloads.constEnd()) {
        *pPayload = *it;
        *pFound = true;
        return true;
    }
    // Absent from a mirror that could not be completed from the drive says
    // nothing about the drive.
    return pMirror->complete;
}

// static
FsCueOverrideStore::Mirror* FsCueOverrideStore::loadedMirror(const FsStoreTarget& target) {
    const QString rootKey = QDir::cleanPath(target.rootPath);
    auto it = s_mirrors.find(rootKey);
    if (it != s_mirrors.end() && it->complete) {
        return &it.value();
    }

    // The one read this drive gets, unless it fails. It runs on whichever
    // thread asks first, under the lock. Normally no write is in flight on the
    // file meanwhile, since a write needs the mirror to exist. The exceptions
    // are a clear, which drops the mirror again once the file is deleted, so
    // nothing read during it is kept, an eject whose flush ran out of time,
    // when the drive is on its way out anyway, and a retry after a failed
    // read. Such a read waits on SQLite's lock, as every read did before the
    // mirror.
    QHash<QString, QByteArray> stored;
    if (!readStoredOverrides(target, &stored)) {
        // Not cached, so the next access retries. What this unit has written
        // to the drive since is still worth answering with.
        return it != s_mirrors.end() ? &it.value() : nullptr;
    }
    if (it == s_mirrors.end()) {
        it = s_mirrors.insert(rootKey, Mirror());
    }
    // Completing a mirror that so far held only this unit's writes: those are
    // newer than anything on the drive, or the same, so they win.
    for (auto row = stored.constBegin(); row != stored.constEnd(); ++row) {
        if (!it->payloads.contains(row.key())) {
            it->payloads.insert(row.key(), row.value());
        }
    }
    it->complete = true;
    return &it.value();
}

// static
FsCueOverrideStore::Mirror& FsCueOverrideStore::mirrorForWrite(const FsStoreTarget& target) {
    Mirror* pMirror = loadedMirror(target);
    if (pMirror) {
        return *pMirror;
    }
    // The store cannot be read right now. The write goes ahead all the same
    // (INSERT OR REPLACE touches only its own row) into a mirror marked
    // incomplete, which a later read completes from the drive.
    return s_mirrors[QDir::cleanPath(target.rootPath)];
}

// static
bool FsCueOverrideStore::takePendingWrite(
        const QString& rootKey, const QString& relPath, QByteArray* pPayload) {
    QMutexLocker locker(&s_baselineMutex);
    const auto it = s_mirrors.find(rootKey);
    if (it == s_mirrors.end() || !it->dirty.remove(relPath)) {
        return false;
    }
    *pPayload = it->payloads.value(relPath);
    return true;
}

// static
void FsCueOverrideStore::markUnsaved(const QString& rootKey, const QString& relPath) {
    QMutexLocker locker(&s_baselineMutex);
    const auto it = s_mirrors.find(rootKey);
    if (it != s_mirrors.end()) {
        it->dirty.insert(relPath);
    }
}

FsStoreWriteResult FsCueOverrideStore::writeOverride(const QString& trackLocation,
        const QString& rootKey,
        const QByteArray& payload) {
    // Resolved again rather than trusting the target of the save: by now the
    // drive may have been ejected, and its mount point left behind as a plain
    // directory on the boot volume.
    FsStoreTarget target;
    if (!FsStoreTarget::resolveForFile(trackLocation, kStoreDbName, &target) ||
            QDir::cleanPath(target.rootPath) != rootKey) {
        return FsStoreWriteResult::Failed;
    }
    if (!target.writable) {
        // A write-protected stick is an expected case, not a failure worth
        // logging on every save.
        return FsStoreWriteResult::Failed;
    }
    {
        // Checked as late as possible: a clear that dropped the mirror after
        // this write was taken may already have deleted the database, and
        // opening it with the DDL below would create it again.
        QMutexLocker locker(&s_baselineMutex);
        if (!s_mirrors.contains(rootKey)) {
            return FsStoreWriteResult::Dropped;
        }
    }

    ScopedFsStore store(kLogTag);
    if (!store.open(target, kCreateTableDdl)) {
        return FsStoreWriteResult::Failed;
    }

    QSqlQuery query(store.database());
    query.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO cue_overrides "
            "(relpath, version, updated_at, cues) "
            "VALUES (:relpath, :version, :updated_at, :cues)"));
    query.bindValue(QStringLiteral(":relpath"), target.relPath);
    query.bindValue(QStringLiteral(":version"), kPayloadVersion);
    query.bindValue(QStringLiteral(":updated_at"),
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    query.bindValue(QStringLiteral(":cues"), QString::fromUtf8(payload));
    if (!query.exec()) {
        qWarning() << "FsCueOverrideStore: cannot save cues for" << target.relPath
                   << query.lastError().text();
        return FsStoreWriteResult::Failed;
    }
    qDebug() << "FsCueOverrideStore: saved cue override for" << target.relPath;
    return FsStoreWriteResult::Written;
}

// static
bool FsCueOverrideStore::clearFilesystemOverrides(const QString& mountPoint) {
    const QString rootKey = QDir::cleanPath(mountPoint);
    {
        // Dropped first, so every write still queued for this drive finds
        // nothing to do instead of recreating the database deleted below.
        QMutexLocker locker(&s_baselineMutex);
        s_mirrors.remove(rootKey);
    }
    // The one the writer may be in the middle of is waited out, so the delete
    // does not land underneath it.
    FsStoreWriter::flushFilesystem(
            rootKey, QDeadlineTimer(FsStoreWriter::kFlushTimeoutMillis));
    const bool removed = fsStoreRemove(mountPoint, kStoreDbName, kLogTag);
    {
        // A read in the meantime may have loaded the mirror again, from the
        // file that is now gone.
        QMutexLocker locker(&s_baselineMutex);
        s_mirrors.remove(rootKey);
    }
    return removed;
}

// static
void FsCueOverrideStore::forgetFilesystem(const QString& mountPoint) {
    QMutexLocker locker(&s_baselineMutex);
    const auto it = s_mirrors.find(QDir::cleanPath(mountPoint));
    if (it == s_mirrors.end()) {
        return;
    }
    if (!it->dirty.isEmpty()) {
        qWarning() << kLogTag << ": discarding" << it->dirty.size()
                   << "unsaved cue override(s) that never reached" << mountPoint
                   << ":" << it->dirty.values();
    }
    s_mirrors.erase(it);
}

// static
bool FsCueOverrideStore::hasMirrorForTesting(const QString& mountPoint) {
    QMutexLocker locker(&s_baselineMutex);
    return s_mirrors.contains(QDir::cleanPath(mountPoint));
}

// static
void FsCueOverrideStore::suppressPendingSaves() {
    QMutexLocker locker(&s_baselineMutex);
    for (auto it = s_baselines.begin(); it != s_baselines.end(); ++it) {
        *it = kSuppressedBaseline;
    }
}

// static
QStringList FsCueOverrideStore::overriddenLocations() {
    QMutexLocker locker(&s_baselineMutex);
    return s_importedCues.keys();
}

// static
bool FsCueOverrideStore::restoreImportedCues(Track* pTrack) {
    VERIFY_OR_DEBUG_ASSERT(pTrack) {
        return false;
    }
    QByteArray imported;
    {
        QMutexLocker locker(&s_baselineMutex);
        const auto it = s_importedCues.constFind(pTrack->getLocation());
        if (it == s_importedCues.constEnd()) {
            // No override was ever applied to this track, so it carries none of
            // this unit's cues and there is nothing for a clear to take off.
            return false;
        }
        imported = *it;
    }
    // An empty payload is meaningful here too: the source library exported no
    // cues at all, so every cue on the track is one the DJ added on this unit
    // and all of them go.
    applyPayload(pTrack, imported);
    return true;
}
