#include "library/dao/fsmetaoverridestore.h"

#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>
#include <QSqlError>
#include <QSqlQuery>
#include <optional>
#include <utility>

#include "library/dao/fsstore.h"
#include "library/dao/fsstorewriter.h"
#include "moc_fsmetaoverridestore.cpp"
#include "preferences/systemsettings.h"
#include "track/track.h"
#include "track/trackrecord.h"
#include "util/assert.h"

namespace {

// Per-filesystem layout, alongside the analysis cache and the cue overrides in
// the same .bitedj dir.
const QString kStoreDbName = QStringLiteral("meta.sqlite");
const char kLogTag[] = "FsMetaOverrideStore";

const QString kCreateTableDdl = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS meta_overrides ("
        "relpath TEXT PRIMARY KEY NOT NULL, "
        "version INTEGER NOT NULL, "
        "updated_at TEXT, "
        "rating INTEGER NOT NULL)");

// Payload format version, so a future change to the stored fields (a comment, a
// track colour) can be told apart from the current one instead of being misread.
constexpr int kPayloadVersion = 1;

// Baseline marker for a track whose override was cleared while it stayed
// loaded. Outside the valid rating range, so it can never equal a real one.
constexpr int kSuppressedBaseline = -1;

bool isStorableRating(int rating) {
    return mixxx::TrackRecord::isValidRating(rating);
}

/// Every current-version rating in the store at `target`, unfiltered, keyed by
/// relative path. True with nothing read when the drive has no store (the
/// common case for a stick nothing has been rated on); false when a store is
/// there but could not be opened or read. An empty DDL writes nothing.
bool readStoredRatings(const FsStoreTarget& target, QHash<QString, int>* pRows) {
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
            "SELECT relpath, rating FROM meta_overrides WHERE version = :version"));
    query.bindValue(QStringLiteral(":version"), kPayloadVersion);
    if (!query.exec()) {
        // Also the path taken by a store written by a future schema.
        qDebug() << kLogTag << ": cannot read overrides on" << target.rootPath
                 << query.lastError().text();
        return false;
    }
    while (query.next()) {
        pRows->insert(query.value(0).toString(), query.value(1).toInt());
    }
    return true;
}

} // anonymous namespace

// static
FsMetaOverrideNotifier& FsMetaOverrideNotifier::instance() {
    static FsMetaOverrideNotifier notifier;
    return notifier;
}

QMutex FsMetaOverrideStore::s_baselineMutex;
QHash<QString, int> FsMetaOverrideStore::s_baselines;
QHash<QString, int> FsMetaOverrideStore::s_importedRatings;
QHash<QString, FsMetaOverrideStore::Mirror> FsMetaOverrideStore::s_mirrors;

int FsMetaOverrideStore::MountRatings::ratingFor(
        const QString& trackLocation, int fallback) const {
    if (rootPath.isEmpty() || byRelPath.isEmpty()) {
        return fallback;
    }
    const auto it = byRelPath.constFind(QDir(rootPath).relativeFilePath(trackLocation));
    return it == byRelPath.constEnd() ? fallback : *it;
}

// static
void FsMetaOverrideStore::applyOverrides(Track* pTrack) {
    VERIFY_OR_DEBUG_ASSERT(pTrack) {
        return;
    }
    const QString location = pTrack->getLocation();
    if (location.isEmpty() || !SystemSettings::isOnRemovableMedia(location)) {
        // A track on the internal drive keeps its rating in the library, where
        // it was already; there is no drive for it to follow.
        return;
    }

    int rating = mixxx::TrackRecord::kNoRating;
    bool found = false;
    // What the source library exported for this track, captured before the
    // override is laid over it. Settings → Clear puts exactly this back, so
    // clearing takes off the DJ's own edit and nothing else.
    const int imported = pTrack->getRating();
    bool hasImported = false;
    if (readOverride(location, &rating, &found) && found && isStorableRating(rating)) {
        hasImported = true;
        pTrack->setRating(rating);
    }

    // Baseline what the track carries now, override applied or not, so that
    // only an edit made from here on counts as a change worth storing.
    const int baseline = pTrack->getRating();
    QMutexLocker locker(&s_baselineMutex);
    s_baselines.insert(location, baseline);
    // Only the first override this track gets: a track is applied to more than
    // once (the database load, then every load out of a Rekordbox playlist),
    // and by the second time the rating on it is this unit's own.
    if (hasImported && !s_importedRatings.contains(location)) {
        s_importedRatings.insert(location, imported);
    }
}

// static
void FsMetaOverrideStore::flushIfChanged(const Track& track) {
    const QString location = track.getLocation();
    if (location.isEmpty() || !SystemSettings::isOnRemovableMedia(location)) {
        return;
    }
    const int rating = track.getRating();
    if (!isStorableRating(rating)) {
        return;
    }

    {
        QMutexLocker locker(&s_baselineMutex);
        const auto it = s_baselines.constFind(location);
        if (it == s_baselines.constEnd()) {
            // Never seen by applyOverrides(): treat the baseline as unrated, so
            // a track first rated in this session is stored, but one that
            // carries no stars at all does not get an entry of its own.
            if (rating == mixxx::TrackRecord::kNoRating) {
                return;
            }
        } else if (*it == kSuppressedBaseline || *it == rating) {
            return;
        }
    }

    queueWrite(location, rating, WriteOrigin::TrackSave);
}

// static
bool FsMetaOverrideStore::storeRating(const QString& trackLocation, int rating) {
    if (trackLocation.isEmpty() || !isStorableRating(rating) ||
            !SystemSettings::isOnRemovableMedia(trackLocation)) {
        return false;
    }
    return queueWrite(trackLocation, rating, WriteOrigin::RatingEdit);
}

// static
bool FsMetaOverrideStore::queueWrite(
        const QString& trackLocation, int rating, WriteOrigin origin) {
    FsStoreTarget target;
    if (!FsStoreTarget::resolveForFile(trackLocation, kStoreDbName, &target) ||
            !target.writable) {
        // Unavailable, or a write-protected stick: an expected case, not a
        // failure worth logging on every save.
        return false;
    }
    const QString rootKey = QDir::cleanPath(target.rootPath);
    const QString relPath = target.relPath;
    std::optional<int> previous;
    {
        QMutexLocker locker(&s_baselineMutex);
        const auto it = s_baselines.constFind(trackLocation);
        if (it != s_baselines.constEnd()) {
            // A track save is checked again: the lock was let go while the
            // target resolved, and a clear may have suppressed the track in
            // that window. An edit in a Rekordbox view is the DJ asking, so
            // it is stored whatever the baseline says.
            if (origin == WriteOrigin::TrackSave &&
                    (*it == kSuppressedBaseline || *it == rating)) {
                return false;
            }
            previous = *it;
        }
        // The track may be loaded in a deck; baseline it at what the drive is
        // about to hold, now rather than once the write lands, so its next
        // save does not queue the same rating again.
        s_baselines.insert(trackLocation, rating);
        Mirror& mirror = mirrorForWrite(target);
        mirror.ratings.insert(relPath, rating);
        mirror.dirty.insert(relPath);
    }
    // Announced now, outside the lock, rather than once the write lands: the
    // Rekordbox view repaints its stars off this, and every read of the store
    // already returns the new rating. Announcing from the write would also let
    // a rating queued just before Settings -> Clear put its stars back after
    // the clear.
    emit FsMetaOverrideNotifier::instance().ratingStored(trackLocation, rating);
    // A deck edit that fails to land is not worth a notification: the next
    // save of the track retries it, as it always has. The view that made a
    // rating edit has let go of it by the time the write lands, so a failure
    // then is announced by ratingStoreFailed().
    const bool reportFailure = origin == WriteOrigin::RatingEdit;
    // Submitted outside the lock, for the reasons given in
    // FsCueOverrideStore::flushIfChanged().
    FsStoreWriter::submit(rootKey,
            [trackLocation, rootKey, relPath, previous, reportFailure]() {
                int written = mixxx::TrackRecord::kNoRating;
                if (!takePendingWrite(rootKey, relPath, &written)) {
                    // An earlier task already wrote this one along with its
                    // own, or the drive's store was cleared or the drive
                    // ejected since.
                    return;
                }
                const FsStoreWriteResult result =
                        writeOverride(trackLocation, rootKey, written);
                if (result != FsStoreWriteResult::Failed) {
                    return;
                }
                qWarning() << kLogTag << ": could not save the rating of" << trackLocation
                           << "to the drive; it stays for this session and the "
                              "next save of the track tries again";
                markUnsaved(rootKey, relPath);
                {
                    // Put the baseline back, so the next save of this track
                    // retries. Only if it is still the rating that failed: a
                    // newer edit, or a clear, has moved it on since.
                    QMutexLocker locker(&s_baselineMutex);
                    const auto it = s_baselines.constFind(trackLocation);
                    if (it != s_baselines.constEnd() && *it == written) {
                        if (previous) {
                            s_baselines.insert(trackLocation, *previous);
                        } else {
                            s_baselines.remove(trackLocation);
                        }
                    }
                }
                if (reportFailure) {
                    emit FsMetaOverrideNotifier::instance().ratingStoreFailed(trackLocation);
                }
            });
    return true;
}

// static
void FsMetaOverrideStore::noteImportedRating(const QString& trackLocation, int rating) {
    if (trackLocation.isEmpty() || !isStorableRating(rating) ||
            !SystemSettings::isOnRemovableMedia(trackLocation)) {
        return;
    }
    QMutexLocker locker(&s_baselineMutex);
    // Only the first one: a second edit would record this unit's own override
    // as the thing to restore.
    if (!s_importedRatings.contains(trackLocation)) {
        s_importedRatings.insert(trackLocation, rating);
    }
}

// static
FsMetaOverrideStore::MountRatings FsMetaOverrideStore::readMountRatings(
        const QString& mountRoot) {
    MountRatings ratings;
    FsStoreTarget target;
    if (!FsStoreTarget::resolveForMount(mountRoot, kStoreDbName, &target)) {
        return ratings;
    }

    QMutexLocker locker(&s_baselineMutex);
    const Mirror* pMirror = loadedMirror(target);
    if (!pMirror) {
        // A store is there but could not be read: nothing read, which the
        // scan takes as no opinion. The next access tries the drive again.
        return ratings;
    }
    // An incomplete mirror gives this unit's own ratings only, which is still
    // better than none.
    for (auto it = pMirror->ratings.constBegin(); it != pMirror->ratings.constEnd(); ++it) {
        if (isStorableRating(it.value())) {
            ratings.byRelPath.insert(it.key(), it.value());
        }
    }
    if (!ratings.byRelPath.isEmpty()) {
        // Left empty for a drive nothing has been rated on, which keeps
        // ratingFor() a pass-through.
        ratings.rootPath = target.rootPath;
    }
    return ratings;
}

// static
bool FsMetaOverrideStore::readOverride(
        const QString& trackLocation, int* pRating, bool* pFound) {
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
    const auto it = pMirror->ratings.constFind(target.relPath);
    if (it != pMirror->ratings.constEnd()) {
        *pRating = *it;
        *pFound = true;
        return true;
    }
    // Absent from a mirror that could not be completed from the drive says
    // nothing about the drive.
    return pMirror->complete;
}

// static
FsMetaOverrideStore::Mirror* FsMetaOverrideStore::loadedMirror(const FsStoreTarget& target) {
    const QString rootKey = QDir::cleanPath(target.rootPath);
    auto it = s_mirrors.find(rootKey);
    if (it != s_mirrors.end() && it->complete) {
        return &it.value();
    }

    // The one read this drive gets unless it fails; see
    // FsCueOverrideStore::loadedMirror().
    QHash<QString, int> stored;
    if (!readStoredRatings(target, &stored)) {
        return it != s_mirrors.end() ? &it.value() : nullptr;
    }
    if (it == s_mirrors.end()) {
        it = s_mirrors.insert(rootKey, Mirror());
    }
    // This unit's own writes are newer than the drive's rows, or the same.
    for (auto row = stored.constBegin(); row != stored.constEnd(); ++row) {
        if (!it->ratings.contains(row.key())) {
            it->ratings.insert(row.key(), row.value());
        }
    }
    it->complete = true;
    return &it.value();
}

// static
FsMetaOverrideStore::Mirror& FsMetaOverrideStore::mirrorForWrite(const FsStoreTarget& target) {
    Mirror* pMirror = loadedMirror(target);
    if (pMirror) {
        return *pMirror;
    }
    // Unreadable right now: the write goes into an incomplete mirror.
    return s_mirrors[QDir::cleanPath(target.rootPath)];
}

// static
bool FsMetaOverrideStore::takePendingWrite(
        const QString& rootKey, const QString& relPath, int* pRating) {
    QMutexLocker locker(&s_baselineMutex);
    const auto it = s_mirrors.find(rootKey);
    if (it == s_mirrors.end() || !it->dirty.remove(relPath)) {
        return false;
    }
    *pRating = it->ratings.value(relPath, mixxx::TrackRecord::kNoRating);
    return true;
}

// static
void FsMetaOverrideStore::markUnsaved(const QString& rootKey, const QString& relPath) {
    QMutexLocker locker(&s_baselineMutex);
    const auto it = s_mirrors.find(rootKey);
    if (it != s_mirrors.end()) {
        it->dirty.insert(relPath);
    }
}

// static
FsStoreWriteResult FsMetaOverrideStore::writeOverride(
        const QString& trackLocation, const QString& rootKey, int rating) {
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
        // Checked as late as possible, for the reason given in
        // FsCueOverrideStore::writeOverride().
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
            "INSERT OR REPLACE INTO meta_overrides "
            "(relpath, version, updated_at, rating) "
            "VALUES (:relpath, :version, :updated_at, :rating)"));
    query.bindValue(QStringLiteral(":relpath"), target.relPath);
    query.bindValue(QStringLiteral(":version"), kPayloadVersion);
    query.bindValue(QStringLiteral(":updated_at"),
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    query.bindValue(QStringLiteral(":rating"), rating);
    if (!query.exec()) {
        qWarning() << kLogTag << ": cannot save rating for" << target.relPath
                   << query.lastError().text();
        return FsStoreWriteResult::Failed;
    }
    qDebug() << kLogTag << ": saved rating" << rating << "for" << target.relPath;
    return FsStoreWriteResult::Written;
}

// static
bool FsMetaOverrideStore::clearFilesystemOverrides(const QString& mountPoint) {
    // The same sequence as FsCueOverrideStore::clearFilesystemOverrides().
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
        // A read in the meantime (the rekordbox scan, say) may have loaded the
        // mirror again, from the file that is now gone.
        QMutexLocker locker(&s_baselineMutex);
        s_mirrors.remove(rootKey);
    }
    return removed;
}

// static
void FsMetaOverrideStore::forgetFilesystem(const QString& mountPoint) {
    QMutexLocker locker(&s_baselineMutex);
    const auto it = s_mirrors.find(QDir::cleanPath(mountPoint));
    if (it == s_mirrors.end()) {
        return;
    }
    if (!it->dirty.isEmpty()) {
        qWarning() << kLogTag << ": discarding" << it->dirty.size()
                   << "unsaved rating(s) that never reached" << mountPoint << ":"
                   << it->dirty.values();
    }
    s_mirrors.erase(it);
}

// static
void FsMetaOverrideStore::suppressPendingSaves() {
    QMutexLocker locker(&s_baselineMutex);
    for (auto it = s_baselines.begin(); it != s_baselines.end(); ++it) {
        *it = kSuppressedBaseline;
    }
}

// static
QStringList FsMetaOverrideStore::overriddenLocations() {
    QMutexLocker locker(&s_baselineMutex);
    return s_importedRatings.keys();
}

// static
bool FsMetaOverrideStore::restoreImportedRating(Track* pTrack) {
    VERIFY_OR_DEBUG_ASSERT(pTrack) {
        return false;
    }
    int imported = mixxx::TrackRecord::kNoRating;
    {
        QMutexLocker locker(&s_baselineMutex);
        const auto it = s_importedRatings.constFind(pTrack->getLocation());
        if (it == s_importedRatings.constEnd()) {
            // No override was ever applied to this track, so it carries none of
            // this unit's ratings and there is nothing for a clear to take off.
            return false;
        }
        imported = *it;
    }
    // An unrated import is meaningful here too: the source library exported no
    // stars at all, so every one on the track was put there on this unit.
    pTrack->setRating(imported);
    return true;
}
