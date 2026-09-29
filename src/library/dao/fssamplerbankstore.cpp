#include "library/dao/fssamplerbankstore.h"

#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QMutexLocker>
#include <QSqlError>
#include <QSqlQuery>
#include <utility>

#include "library/dao/fsstore.h"
#include "library/dao/fsstorewriter.h"

namespace {

// Per-filesystem layout, alongside the analysis cache and the cue overrides in
// the same .bitedj dir.
const QString kStoreDbName = QStringLiteral("samplers.sqlite");
const char kLogTag[] = "FsSamplerBankStore";

// Payload format version, so a future change to the stored fields can be told
// apart from the current one instead of being misread.
constexpr int kPayloadVersion = 1;

const QString kCreateTableDdl = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS sampler_banks ("
        "bank INTEGER PRIMARY KEY NOT NULL, "
        "version INTEGER NOT NULL, "
        "updated_at TEXT, "
        "slots TEXT NOT NULL)");

/// Every current-version bank in the store at `target`, keyed by bank index.
/// True with nothing read when the drive has no store (the common case for a
/// stick that has never been used here); false when a store is there but could
/// not be opened or read. An empty DDL writes nothing.
bool readStoredBanks(const FsStoreTarget& target, QHash<int, QByteArray>* pRows) {
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
            "SELECT bank, slots FROM sampler_banks WHERE version = :version"));
    query.bindValue(QStringLiteral(":version"), kPayloadVersion);
    if (!query.exec()) {
        // Also the path taken by a store written by a future schema.
        qDebug() << kLogTag << ": cannot read banks on" << target.rootPath
                 << query.lastError().text();
        return false;
    }
    while (query.next()) {
        pRows->insert(query.value(0).toInt(), query.value(1).toString().toUtf8());
    }
    return true;
}

} // anonymous namespace

QMutex FsSamplerBankStore::s_mutex;
QHash<QString, FsSamplerBankStore::Mirror> FsSamplerBankStore::s_mirrors;
QHash<QString, QDeadlineTimer> FsSamplerBankStore::s_loadRetryAfter;
QHash<QString, quint64> FsSamplerBankStore::s_dropGenerations;

// static
QByteArray FsSamplerBankStore::serializeBank(
        const QString& mountRoot, const QStringList& locations) {
    const QDir rootDir(mountRoot);
    QJsonArray array;
    for (const QString& location : locations) {
        if (location.isEmpty()) {
            array.append(QString());
            continue;
        }
        const QString absPath = QFileInfo(location).absoluteFilePath();
        // A sample that lives on the same stick is stored relative to its root
        // so the bank still resolves when the drive is mounted elsewhere, or on
        // another unit. One that does not (a track on the boot volume, or on a
        // second stick) can only be named absolutely; it simply fails to load
        // where that path does not exist, which is the honest outcome.
        const QString relPath = rootDir.relativeFilePath(absPath);
        const bool onThisDrive = !relPath.isEmpty() &&
                !relPath.startsWith(QLatin1String("..")) &&
                !QDir::isAbsolutePath(relPath);
        array.append(onThisDrive ? relPath : absPath);
    }
    return QJsonDocument(array).toJson(QJsonDocument::Compact);
}

// static
QStringList FsSamplerBankStore::parseBank(
        const QString& mountRoot, const QByteArray& payload, int slotCount) {
    QStringList locations;
    locations.reserve(slotCount);

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        qWarning() << kLogTag << ": ignoring unreadable sampler bank on" << mountRoot
                   << parseError.errorString();
        return QStringList(slotCount, QString());
    }

    const QDir rootDir(mountRoot);
    const QJsonArray array = document.array();
    for (const QJsonValue& value : array) {
        if (locations.size() >= slotCount) {
            // A bank stored by a build with a bigger grid: keep the slots this
            // one has room for rather than refusing the whole bank.
            break;
        }
        const QString stored = value.toString();
        if (stored.isEmpty()) {
            locations.append(QString());
        } else if (QDir::isAbsolutePath(stored)) {
            locations.append(QDir::cleanPath(stored));
        } else {
            locations.append(rootDir.absoluteFilePath(stored));
        }
    }
    while (locations.size() < slotCount) {
        locations.append(QString());
    }
    return locations;
}

// static
bool FsSamplerBankStore::readBank(const QString& mountRoot,
        int bankIndex,
        int slotCount,
        QStringList* pLocations) {
    FsStoreTarget target;
    if (!FsStoreTarget::resolveForMount(mountRoot, kStoreDbName, &target)) {
        return false;
    }

    ensureLoaded(target);
    QByteArray payload;
    {
        QMutexLocker locker(&s_mutex);
        const auto mirror = s_mirrors.constFind(target.rootPath);
        if (mirror == s_mirrors.constEnd()) {
            // A store is there but could not be read; a later read tries the
            // drive again.
            return false;
        }
        const auto it = mirror->payloads.constFind(bankIndex);
        if (it == mirror->payloads.constEnd()) {
            // No bank at this index on this drive (the common case for a stick
            // that has never been used here), or none this unit has written to
            // a store it could not read.
            return false;
        }
        payload = *it;
    }
    *pLocations = parseBank(target.rootPath, payload, slotCount);
    return true;
}

// static
bool FsSamplerBankStore::writeBank(const QString& mountRoot,
        int bankIndex,
        const QStringList& locations) {
    FsStoreTarget target;
    if (!FsStoreTarget::resolveForMount(mountRoot, kStoreDbName, &target)) {
        return false;
    }
    if (!target.writable) {
        // A write-protected stick is an expected case, not a failure worth
        // logging every time a slot changes.
        return false;
    }

    // resolveForMount() stores the cleaned root, so this is already the key
    // the mirror and the writer's pending count go by.
    const QString rootKey = target.rootPath;
    const QByteArray payload = serializeBank(target.rootPath, locations);
    // Before the lock, since it may read the drive. An unreadable store still
    // takes the write, into an incomplete mirror.
    ensureLoaded(target);
    {
        QMutexLocker locker(&s_mutex);
        Mirror& mirror = s_mirrors[rootKey];
        mirror.payloads.insert(bankIndex, payload);
        mirror.dirty.insert(bankIndex);
    }
    // Submitted outside the lock, for the reasons given in
    // FsCueOverrideStore::flushIfChanged().
    FsStoreWriter::submit(rootKey, [rootKey, bankIndex]() {
        QByteArray written;
        if (!takePendingWrite(rootKey, bankIndex, &written)) {
            // An earlier task already wrote this bank along with its own, or
            // the drive's banks were cleared or the drive ejected since.
            return;
        }
        if (writePayload(rootKey, bankIndex, written) != FsStoreWriteResult::Failed) {
            return;
        }
        // No baseline to put back: SamplerDrive took this row as its baseline
        // when the write was accepted, and the next change to the bank writes
        // the whole row again. Until then the store keeps showing it, and
        // forgetFilesystem() reports it if it never lands.
        qWarning() << kLogTag << ": could not save sampler bank" << bankIndex << "on"
                   << rootKey << "; the next change to the bank tries again";
        markUnsaved(rootKey, bankIndex);
    });
    return true;
}

// static
void FsSamplerBankStore::ensureLoaded(const FsStoreTarget& target) {
    const QString rootKey = QDir::cleanPath(target.rootPath);
    quint64 generation = 0;
    {
        QMutexLocker locker(&s_mutex);
        const auto it = s_mirrors.constFind(rootKey);
        if (it != s_mirrors.constEnd() && it->complete) {
            return;
        }
        const auto retryAfter = s_loadRetryAfter.constFind(rootKey);
        if (retryAfter != s_loadRetryAfter.constEnd() && !retryAfter->hasExpired()) {
            // Failed a moment ago; not worth waiting on the drive for again.
            return;
        }
        generation = s_dropGenerations.value(rootKey);
    }

    // The one read this drive gets unless it fails, made with the lock let
    // go; see FsCueOverrideStore::ensureLoaded().
    QHash<int, QByteArray> stored;
    const bool read = readStoredBanks(target, &stored);

    QMutexLocker locker(&s_mutex);
    if (s_dropGenerations.value(rootKey) != generation) {
        // Dropped by a clear or an eject while the read ran: not kept.
        return;
    }
    auto it = s_mirrors.find(rootKey);
    if (it != s_mirrors.end() && it->complete) {
        // Another thread's read got there first.
        return;
    }
    if (!read) {
        s_loadRetryAfter.insert(rootKey, QDeadlineTimer(kFsStoreLoadRetryMillis));
        return;
    }
    s_loadRetryAfter.remove(rootKey);
    if (it == s_mirrors.end()) {
        it = s_mirrors.insert(rootKey, Mirror());
    }
    // This unit's own writes are newer than the drive's rows, or the same.
    for (auto row = stored.constBegin(); row != stored.constEnd(); ++row) {
        if (!it->payloads.contains(row.key())) {
            it->payloads.insert(row.key(), row.value());
        }
    }
    it->complete = true;
}

// static
void FsSamplerBankStore::dropMirror(const QString& rootKey) {
    s_mirrors.remove(rootKey);
    s_loadRetryAfter.remove(rootKey);
    ++s_dropGenerations[rootKey];
}

// static
void FsSamplerBankStore::markUnsaved(const QString& rootKey, int bankIndex) {
    QMutexLocker locker(&s_mutex);
    const auto it = s_mirrors.find(rootKey);
    if (it != s_mirrors.end()) {
        it->dirty.insert(bankIndex);
    }
}

// static
bool FsSamplerBankStore::takePendingWrite(
        const QString& rootKey, int bankIndex, QByteArray* pPayload) {
    QMutexLocker locker(&s_mutex);
    const auto it = s_mirrors.find(rootKey);
    if (it == s_mirrors.end() || !it->dirty.remove(bankIndex)) {
        return false;
    }
    *pPayload = it->payloads.value(bankIndex);
    return true;
}

// static
FsStoreWriteResult FsSamplerBankStore::writePayload(
        const QString& rootKey, int bankIndex, const QByteArray& payload) {
    // Resolved again: by now the drive may have been ejected, and a stale
    // mount point resolves to nothing rather than to the boot volume.
    FsStoreTarget target;
    if (!FsStoreTarget::resolveForMount(rootKey, kStoreDbName, &target) ||
            !target.writable) {
        return FsStoreWriteResult::Failed;
    }
    {
        // Checked as late as possible, for the reason given in
        // FsCueOverrideStore::writeOverride().
        QMutexLocker locker(&s_mutex);
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
            "INSERT OR REPLACE INTO sampler_banks "
            "(bank, version, updated_at, slots) "
            "VALUES (:bank, :version, :updated_at, :slots)"));
    query.bindValue(QStringLiteral(":bank"), bankIndex);
    query.bindValue(QStringLiteral(":version"), kPayloadVersion);
    query.bindValue(QStringLiteral(":updated_at"),
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    query.bindValue(QStringLiteral(":slots"), QString::fromUtf8(payload));
    if (!query.exec()) {
        qWarning() << kLogTag << ": cannot save bank" << bankIndex << "on" << rootKey
                   << query.lastError().text();
        return FsStoreWriteResult::Failed;
    }
    qDebug() << kLogTag << ": saved sampler bank" << bankIndex << "on" << rootKey;
    return FsStoreWriteResult::Written;
}

// static
bool FsSamplerBankStore::clearFilesystemBanks(const QString& mountPoint) {
    // The same sequence as FsCueOverrideStore::clearFilesystemOverrides().
    const QString rootKey = QDir::cleanPath(mountPoint);
    {
        // Dropped first, so every write still queued for this drive finds
        // nothing to do instead of recreating the database deleted below.
        QMutexLocker locker(&s_mutex);
        dropMirror(rootKey);
    }
    // The one the writer may be in the middle of is waited out, so the delete
    // does not land underneath it.
    FsStoreWriter::flushFilesystem(
            rootKey, QDeadlineTimer(FsStoreWriter::kFlushTimeoutMillis));
    const bool removed = fsStoreRemove(mountPoint, kStoreDbName, kLogTag);
    {
        // A read in the meantime may have loaded the mirror again, from the
        // file that is now gone, and one still running is kept out by the
        // drop.
        QMutexLocker locker(&s_mutex);
        dropMirror(rootKey);
    }
    return removed;
}

// static
void FsSamplerBankStore::forgetFilesystem(const QString& mountPoint) {
    const QString rootKey = QDir::cleanPath(mountPoint);
    QMutexLocker locker(&s_mutex);
    const auto it = s_mirrors.constFind(rootKey);
    if (it != s_mirrors.constEnd() && !it->dirty.isEmpty()) {
        // Nothing to mark for a retry here: SamplerDrive keeps the baselines,
        // and a drive going away resets them (setMountRoot() and
        // suppressSavesTo() both do), after which the grid is reloaded from
        // what the drive really holds when it is back. Should the same stick
        // be back under the same mount point before SamplerDrive saw it go,
        // the grid keeps the lost row and the next change to that bank writes
        // the whole row, as after a failed write.
        qWarning() << kLogTag << ": discarding" << it->dirty.size()
                   << "unsaved sampler bank(s) that never reached" << mountPoint
                   << ":" << it->dirty.values();
    }
    // Dropped even with no mirror, so a read still in flight is not kept.
    dropMirror(rootKey);
}
