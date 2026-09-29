#pragma once

#include <QDeadlineTimer>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

class Track;
struct FsStoreTarget;
enum class FsStoreWriteResult;

/// Announces every rating this unit writes to a drive, so the views that show a
/// rating from somewhere other than the track itself can follow it.
///
/// The Rekordbox playlist view is the one that needs it: its ratings come from
/// the device's exported database (mirrored into a temporary table at scan
/// time), not from the Track object, so a rating set on a deck would otherwise
/// not show in the playlist until the drive is scanned again.
class FsMetaOverrideNotifier : public QObject {
    Q_OBJECT

  public:
    /// Lazily created on first use. Callers on the GUI thread (the features
    /// that connect to it) touch it first, so it takes their affinity and a
    /// store write from a worker thread is delivered queued.
    static FsMetaOverrideNotifier& instance();

  signals:
    /// `trackLocation` now has `rating` stored for its drive. Emitted on the
    /// thread that made the edit as soon as it is accepted, which is when
    /// every read of the store starts returning it; the write to the drive
    /// follows in the background.
    void ratingStored(const QString& trackLocation, int rating);

    /// A rating accepted by FsMetaOverrideStore::storeRating() could not be
    /// written to the drive after all. Emitted on the FsStoreWriter thread.
    void ratingStoreFailed(const QString& trackLocation);

  private:
    FsMetaOverrideNotifier() = default;

    friend class FsMetaOverrideStore;
};

/// Portable, per-filesystem store for the track metadata a DJ edits on this
/// unit. Currently that is the star rating.
///
/// A rating changed here is written back to the drive the track came from, into
/// a self-contained SQLite database next to the analysis cache and the cue
/// overrides: `<mountRoot>/.bitedj/meta.sqlite`. Entries are keyed by the
/// track's path relative to the mount root, so the rating travels with the
/// stick and comes back on the next insertion — on this unit or any other Bite
/// DJ one — rather than living in this box's library, which the drive's own
/// playlists are never read from.
///
/// A stored entry is an *override*: it wins over the rating the source library
/// exported (a rekordbox `DJ Rating`), and it is applied both to the Track
/// object and to the scanned copy of that library. An entry holding no rating
/// at all is meaningful — that is a track whose stars the DJ took off — which
/// is why the store distinguishes "no entry" from "an entry of zero".
///
/// Like FsCueOverrideStore, nothing here touches a drive's SQLite on the
/// calling thread after the first access to it: that access loads every rating
/// on the drive into an in-memory mirror, outside the store's mutex, so the
/// rekordbox scan's first read of a stick does not hold up a rating edit or a
/// track load on the GUI thread. Reads are answered from the mirror, and
/// writes update it and are queued on FsStoreWriter, which stores whatever the
/// mirror holds when it gets to them. A store that cannot be read is left
/// alone for kFsStoreLoadRetryMillis. Each write still opens the database,
/// runs one statement and closes it again, so no file descriptor lingers to
/// make `umount` fail with EBUSY on eject.
///
/// All members are static: the baselines and mirrors below are process-wide
/// state guarded by an internal mutex, and every entry point is safe to call
/// from any thread.
class FsMetaOverrideStore {
  public:
    /// Every rating stored for one drive, read in a single pass.
    ///
    /// The rekordbox scan needs the whole picture at once — it walks thousands
    /// of tracks and cannot open the store per track — so it takes one of these
    /// and asks it per file.
    struct MountRatings {
        /// Mount root the relative paths below are keyed against. Empty when
        /// the drive has no store (or is not a removable one), which makes
        /// ratingFor() a pass-through.
        QString rootPath;
        QHash<QString, int> byRelPath;

        bool isEmpty() const {
            return byRelPath.isEmpty();
        }

        /// The stored rating for the file at `trackLocation`, or `fallback`
        /// (what the source library says) when the DJ never rated it here.
        int ratingFor(const QString& trackLocation, int fallback) const;
    };

    /// Apply the stored rating for `pTrack`, if the track's filesystem has one,
    /// and remember the resulting rating as the baseline against which a later
    /// flushIfChanged() detects DJ edits.
    ///
    /// As in FsCueOverrideStore::applyOverrides(), a baseline marked unsaved
    /// by forgetFilesystem() is kept, and a track loaded while its drive's
    /// store is there but cannot be read is baselined as suppressed, so it
    /// saves nothing until a later load reads the store.
    static void applyOverrides(Track* pTrack);

    /// Queue the track's rating to be written to its filesystem if it differs
    /// from the baseline remembered by applyOverrides(), i.e. if the DJ changed
    /// it since the track was loaded. Returns without waiting for the drive,
    /// having emitted ratingStored(); a failed write puts the baseline back so
    /// a later save tries again. Queues nothing at all when nothing changed,
    /// which is what keeps eject free of EBUSY.
    ///
    /// A track that was never seen by applyOverrides() has no baseline, so its
    /// rating is stored the first time it is saved with any stars on it. A
    /// suppressed baseline (after a clear, or a load while the store could not
    /// be read) queues nothing; an unsaved one queues the track's rating
    /// whatever it is.
    static void flushIfChanged(const Track& track);

    /// Store `rating` for `trackLocation` outright, for an edit made somewhere
    /// the Track object is not the thing being edited — the rating cell of a
    /// Rekordbox playlist, whose stars come from the device's own database.
    /// Returns false when the drive cannot be written to at all (not there, not
    /// removable, write-protected). Otherwise the write is queued and this
    /// returns true at once, having emitted ratingStored();
    /// ratingStoreFailed() follows if the drive refuses the write after all.
    static bool storeRating(const QString& trackLocation, int rating);

    /// Remember `rating` as what `trackLocation` carried before this unit's
    /// first override went on it, unless something is already remembered.
    /// Callers that edit a rating without going through applyOverrides() use
    /// this so that clearing can put the track back rather than blank it.
    static void noteImportedRating(const QString& trackLocation, int rating);

    /// Every rating stored on the filesystem mounted at `mountRoot`, including
    /// the ones still queued for it. Returns an empty (pass-through) result for
    /// a drive with no store. Safe to call from a worker thread (the rekordbox
    /// scan does).
    static MountRatings readMountRatings(const QString& mountRoot);

    /// Delete the metadata override database of the filesystem mounted at
    /// `mountPoint` (`<mountPoint>/.bitedj/meta.sqlite`). Writes still queued
    /// for the drive are dropped, and one in progress is waited for (bounded)
    /// before the delete. Returns false only when a database exists but could
    /// not be deleted; a drive without one counts as success.
    static bool clearFilesystemOverrides(const QString& mountPoint);

    /// Forget what this store knows about the drive mounted at `mountPoint`,
    /// for the eject: a stick that comes back may have been edited elsewhere,
    /// so its next access reads it again. Writes still queued for it become
    /// no-ops, so call this only once they have been flushed. As in
    /// FsCueOverrideStore::forgetFilesystem(), the baseline of every track
    /// whose write is lost that way is marked unsaved, so its next save
    /// stores its rating again.
    static void forgetFilesystem(const QString& mountPoint);

    /// Stop flushIfChanged() from re-creating an override for a track that is
    /// still loaded (typically one in a deck) after the overrides were cleared.
    /// The next load of the track re-baselines it and saves again.
    static void suppressPendingSaves();

    /// Locations of the tracks this store has actually put a rating on since
    /// startup. Paired with the global track cache to reach the ones that are
    /// still in a deck when the overrides are cleared.
    ///
    /// Deliberately *not* every track it has baselined: a track the drive held
    /// no override for carries nothing of this unit's, so clearing has nothing
    /// to take off it and must not touch it at all.
    static QStringList overriddenLocations();

    /// Put `pTrack` back to the rating its source library exported — what it
    /// carried before this unit's override went on top. Returns false, changing
    /// nothing, for a track that never had one.
    static bool restoreImportedRating(Track* pTrack);

  private:
    /// Every rating stored on one drive, as loaded from it and updated by this
    /// unit's writes since.
    struct Mirror {
        /// Relative path -> stored rating, unfiltered.
        QHash<QString, int> ratings;
        /// Relative paths whose rating has been queued but not yet written,
        /// or whose write failed and waits for the next save to retry it.
        QSet<QString> dirty;
        /// Relative path -> the track location this unit queued its last
        /// write under, which is the key its baseline has in s_baselines.
        QHash<QString, QString> locations;
        /// False while the drive's store could not be read: the mirror then
        /// holds only this unit's own writes, and every read of a path it does
        /// not hold tries the drive again (once the retry backoff is over).
        bool complete = false;
    };

    /// Where a write comes from, which decides what queueWrite() checks and
    /// reports.
    enum class WriteOrigin {
        /// flushIfChanged(): skipped if the baseline caught up meanwhile, and
        /// a failure is only logged.
        TrackSave,
        /// storeRating(): always queued, and a failure is announced.
        RatingEdit,
    };

    /// What readOverride() found out; see FsCueOverrideStore::StoreRead.
    enum class StoreRead {
        Unavailable,
        Unreadable,
        Absent,
        Found,
    };

    // Answered from the mirror, which is loaded first if need be. `pRating`
    // is set for Found only.
    static StoreRead readOverride(const QString& trackLocation, int* pRating);
    // Baseline `rating` for `trackLocation`, put it in the mirror, emit
    // ratingStored() and queue the write. False, changing nothing, when the
    // drive cannot be written to (or, for a TrackSave, nothing needs writing).
    static bool queueWrite(const QString& trackLocation, int rating, WriteOrigin origin);
    // Load (or complete) the mirror of the drive `target` is on, reading the
    // drive with s_baselineMutex let go; see
    // FsCueOverrideStore::ensureLoaded(). Must be called without the lock.
    static void ensureLoaded(const FsStoreTarget& target);
    // See FsCueOverrideStore::dropMirror(). Requires s_baselineMutex.
    static void dropMirror(const QString& rootKey);
    // Writer thread: take the rating queued for `relPath` on the drive keyed
    // `rootKey`, if it is still waiting to be written. False when an earlier
    // task already wrote it, or the drive's mirror has been dropped.
    static bool takePendingWrite(const QString& rootKey, const QString& relPath, int* pRating);
    // Writer thread: after a failed write, mark `relPath` unsaved again.
    static void markUnsaved(const QString& rootKey, const QString& relPath);
    // Writer thread: store `rating` for the track at `trackLocation`, provided
    // it still resolves to the drive keyed `rootKey` and that drive's mirror
    // has not been dropped in the meantime.
    static FsStoreWriteResult writeOverride(
            const QString& trackLocation, const QString& rootKey, int rating);

    static QMutex s_baselineMutex;
    // Maps a track's location to the rating it was loaded with (or last saved
    // with), to a suppression marker set by suppressPendingSaves(), or to the
    // unsaved marker forgetFilesystem() sets.
    static QHash<QString, int> s_baselines;
    // Maps a track's location to the rating its source library exported, as it
    // stood just before an override was applied over it. Only holds the tracks
    // that actually got one, which is what restoreImportedRating() keys off.
    static QHash<QString, int> s_importedRatings;
    // Maps a drive's cleaned mount root to its mirror. Guarded by
    // s_baselineMutex, which is never held across disk I/O, not even the
    // read that loads a mirror.
    static QHash<QString, Mirror> s_mirrors;
    // See FsCueOverrideStore::s_loadRetryAfter and s_dropGenerations.
    static QHash<QString, QDeadlineTimer> s_loadRetryAfter;
    static QHash<QString, quint64> s_dropGenerations;
};
