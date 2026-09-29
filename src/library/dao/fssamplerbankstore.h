#pragma once

#include <QByteArray>
#include <QDeadlineTimer>
#include <QHash>
#include <QMutex>
#include <QSet>
#include <QString>
#include <QStringList>

struct FsStoreTarget;
enum class FsStoreWriteResult;

/// Portable, per-filesystem store for the sampler banks a DJ builds on this
/// unit.
///
/// The sampler grid is split into two banks of eight (see SamplerDrive), both
/// saved to the one drive the DJ picked, into the self-contained SQLite
/// database `<mountRoot>/.bitedj/samplers.sqlite` next to the analysis cache
/// and the cue overrides. A bank is stored as the list of its slots' file paths
/// — relative to the mount root, since a sample can only be one that lives on
/// that stick, so the whole set travels with the drive and comes back on any
/// other Bite DJ unit.
///
/// A stored bank with no slots filled is meaningful — that is a bank the DJ
/// emptied — which is why the store distinguishes "no bank" from "an empty
/// bank": a drive that has never held a bank is not one whose owner cleared it.
///
/// Like FsCueOverrideStore, nothing here touches a drive's SQLite on the
/// calling thread after the first access to it: that access loads every bank
/// on the drive into an in-memory mirror, outside the store's mutex, reads are
/// answered from the mirror, and writes update it and are queued on
/// FsStoreWriter. A store that cannot be read is left alone for
/// kFsStoreLoadRetryMillis. Each write still opens
/// the database and closes it again; see ScopedFsStore for why a lingering file
/// descriptor would break eject.
///
/// All members are static, and the mirrors are guarded by an internal mutex,
/// so every entry point is safe to call from any thread.
class FsSamplerBankStore {
  public:
    /// Read bank `bankIndex` stored on the drive mounted at `mountRoot` into
    /// `pLocations`, as absolute paths with an empty string for every empty
    /// slot. The list is always resized to `slotCount`: a bank stored by a
    /// build with a different bank size is truncated or padded rather than
    /// rejected. A bank still queued for the drive reads as stored.
    ///
    /// Returns false when the drive is unavailable or holds no bank at that
    /// index, leaving `pLocations` untouched.
    static bool readBank(const QString& mountRoot,
            int bankIndex,
            int slotCount,
            QStringList* pLocations);

    /// Queue `locations` (absolute paths, empty string for an empty slot) to
    /// be stored as bank `bankIndex` on the drive mounted at `mountRoot`.
    /// Returns whether the write was accepted, i.e. the drive is there and
    /// writable; it lands later. A write the drive then refuses is logged and
    /// kept in the store's copy for this session: SamplerDrive has already
    /// taken the row as its baseline, so the next change to that bank writes
    /// it again.
    static bool writeBank(const QString& mountRoot,
            int bankIndex,
            const QStringList& locations);

    /// Delete the sampler bank database of the filesystem mounted at
    /// `mountPoint`. Writes still queued for the drive are dropped, and one in
    /// progress is waited for (bounded) before the delete. Returns false only
    /// when one exists but could not be deleted; a drive without banks counts
    /// as success.
    static bool clearFilesystemBanks(const QString& mountPoint);

    /// Forget what this store knows about the drive mounted at `mountPoint`,
    /// for the eject: a stick that comes back may have been edited elsewhere,
    /// so its next access reads it again. Writes still queued for it become
    /// no-ops, so call this only once they have been flushed.
    static void forgetFilesystem(const QString& mountPoint);

    /// The stored payload: one JSON array of slot paths, relative to
    /// `mountRoot` for the samples that live on that drive and absolute for the
    /// ones that do not (which SamplerDrive no longer allows into a slot, but
    /// a bank written by an older build can still name). Also serves as the comparison value that decides
    /// whether a bank changed and is worth writing, so the same set of slots
    /// always has to serialize to the same bytes.
    ///
    /// Public so the codec can be exercised without a removable drive under the
    /// test, and so SamplerDrive can compare against a bank it just read.
    static QByteArray serializeBank(const QString& mountRoot, const QStringList& locations);

    /// Inverse of serializeBank(): absolute paths, exactly `slotCount` of them.
    static QStringList parseBank(const QString& mountRoot,
            const QByteArray& payload,
            int slotCount);

  private:
    /// Every bank stored on one drive, as loaded from it and updated by this
    /// unit's writes since.
    struct Mirror {
        /// Bank index -> stored payload (see serializeBank()).
        QHash<int, QByteArray> payloads;
        /// Banks whose payload has been queued but not yet written, or whose
        /// write failed and waits for the next change to the bank.
        QSet<int> dirty;
        /// False while the drive's store could not be read: the mirror then
        /// holds only this unit's own writes, and every read of a bank it does
        /// not hold tries the drive again (once the retry backoff is over).
        bool complete = false;
    };

    // Load (or complete) the mirror of the drive `target` is on, reading the
    // drive with s_mutex let go; see FsCueOverrideStore::ensureLoaded(). Must
    // be called without the lock.
    static void ensureLoaded(const FsStoreTarget& target);
    // See FsCueOverrideStore::dropMirror(). Requires s_mutex to be held.
    static void dropMirror(const QString& rootKey);
    // Writer thread: take the payload queued for `bankIndex` on the drive
    // keyed `rootKey`, if it is still waiting to be written. False when an
    // earlier task already wrote it, or the drive's mirror has been dropped.
    static bool takePendingWrite(const QString& rootKey, int bankIndex, QByteArray* pPayload);
    // Writer thread: after a failed write, mark `bankIndex` unsaved again.
    static void markUnsaved(const QString& rootKey, int bankIndex);
    // Writer thread: store `payload` as bank `bankIndex` on the drive mounted
    // at `rootKey`, provided it is still mounted there and its mirror has not
    // been dropped in the meantime.
    static FsStoreWriteResult writePayload(
            const QString& rootKey, int bankIndex, const QByteArray& payload);

    static QMutex s_mutex;
    // Maps a drive's cleaned mount root to its mirror. Guarded by s_mutex,
    // which is never held across disk I/O, not even the read that loads a
    // mirror.
    static QHash<QString, Mirror> s_mirrors;
    // See FsCueOverrideStore::s_loadRetryAfter and s_dropGenerations.
    static QHash<QString, QDeadlineTimer> s_loadRetryAfter;
    static QHash<QString, quint64> s_dropGenerations;
};
