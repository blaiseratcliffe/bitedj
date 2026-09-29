#pragma once

#include <QByteArray>
#include <QDeadlineTimer>
#include <QHash>
#include <QMutex>
#include <QSet>
#include <QString>
#include <QStringList>

class Track;
struct FsStoreTarget;
enum class FsStoreWriteResult;

/// Portable, per-filesystem store for the cues a DJ sets on this unit.
///
/// Hot cues and memory cues edited while a track plays are written back to the
/// drive the track came from, into a self-contained SQLite database next to the
/// analysis cache: `<mountRoot>/.bitedj/cues.sqlite`. Entries are keyed by the
/// track's path relative to the mount root and hold positions in seconds, so
/// the cues travel with the stick and reload on any other Bite DJ unit.
///
/// A stored entry is an *override*: it is the whole picture of a track's hot
/// cue bank, memory cue bank and main cue, and it is applied after the
/// rekordbox ANLZ import, so it wins over whatever rekordbox exported. An entry
/// with no cues in it is meaningful — that is a track whose cues the DJ deleted
/// — which is why the store distinguishes "no entry" from "an empty entry".
///
/// After the first access to a drive nothing here touches that drive's SQLite
/// on the calling thread, which is usually the GUI thread (a track load reads
/// the store twice, and every save of a track off a stick comes through here).
/// A slow stick can take seconds to accept one write, and SQLite locks the file
/// while it does, so a read made then would wait behind it just the same:
/// - The first access to a drive, read or write, loads every override stored
///   on it into an in-memory mirror with one SELECT. Every later read is
///   answered from the mirror. That one read runs on the calling thread but
///   outside the store's mutex, so it holds up only its own caller, not the
///   reads and saves other threads make meanwhile. A store that is there but
///   cannot be read is left alone for kFsStoreLoadRetryMillis before the next
///   access tries it again.
/// - A save updates the mirror, marks the entry dirty and queues the write on
///   FsStoreWriter. The queued write stores whatever the mirror holds for that
///   track when it runs, so saves that pile up behind a slow write coalesce.
/// - Clearing a drive, and ejecting it, drop its mirror, which turns the
///   writes still queued for it into no-ops.
///
/// Unlike FsAnalysisCache this store still does not keep its connections open:
/// every write opens the database, runs one statement and closes it again, now
/// on the writer thread. A lingering file descriptor on a USB stick makes
/// `umount` fail with EBUSY on eject, and the eject path closes the analysis
/// caches *before* pumping the event loop that evicts (and thereby saves) the
/// track; it flushes the writer after that pump, so the save lands and closes
/// its file before the unmount.
///
/// All members are static: the baselines and mirrors below are process-wide
/// state guarded by an internal mutex, and every entry point is safe to call
/// from any thread.
class FsCueOverrideStore {
  public:
    /// Apply the stored cue override for `pTrack`, if the track's filesystem
    /// has one, and remember the resulting cue set as the baseline against
    /// which a later flushIfChanged() detects DJ edits.
    ///
    /// Cues are updated in place wherever a slot survives, because this runs on
    /// the same Track object a deck may already be playing; recreating them
    /// would blank that deck's pads and drop an active saved loop mid-set.
    ///
    /// Does nothing (and records no baseline, so a later load can retry) while
    /// the track has no valid sample rate to convert stored seconds with.
    ///
    /// Two baselines survive it. One marked unsaved by forgetFilesystem() is
    /// kept, so the edit the drive never got is still stored by the next
    /// save. And when the drive's store is there but cannot be read, the
    /// track is baselined as suppressed instead: its cues were loaded without
    /// whatever override the drive holds for it, and saving them would
    /// replace that override with the imported cues plus an edit. Its saves
    /// stay paused until a later load reads the store.
    static void applyOverrides(Track* pTrack);

    /// Queue the track's cues to be written to its filesystem if they differ
    /// from the baseline remembered by applyOverrides(), i.e. if the DJ added,
    /// moved or deleted a cue since the track was loaded. Returns without
    /// waiting for the drive: the baseline moves to the new cues at once, and
    /// goes back if the write fails so that a later save tries again. Queues
    /// nothing at all when nothing changed, which is what keeps eject free of
    /// EBUSY.
    ///
    /// A track that was never baselined by applyOverrides() has an empty
    /// baseline, so its cues are stored the first time it is saved with any
    /// cue set. TrackDAO baselines every track it fetches or adds, so this is
    /// left to a track applyOverrides() skipped because it had no sample rate
    /// yet, and to one built outside the library. A suppressed baseline (after
    /// a clear, or a load while the store could not be read) queues nothing;
    /// an unsaved one queues the track's cues whatever they are.
    static void flushIfChanged(const Track& track);

    /// Move the main cue in the baseline of `track` to the one the track
    /// carries now, inserting or removing it as needed, and leave every other
    /// entry of the baseline exactly as it was. For a main cue that is not the
    /// DJ's: the silence analyzer's first-sound position, which every unit
    /// works out for itself, or a main cue object CueControl creates for a
    /// position the track already had. Without this the next save would see
    /// the cue set differ from the baseline and store all of the imported
    /// cues as a DJ override.
    ///
    /// Only the baseline moves: nothing is written, and the mirror is not
    /// touched. Does nothing for a track with no baseline or a suppressed one,
    /// one off removable media, or one without a valid sample rate. Safe to
    /// call from any thread; the analyzer calls it from its own.
    static void rebaselineMainCue(const Track& track);

    /// Delete the cue override database of the filesystem mounted at
    /// `mountPoint` (`<mountPoint>/.bitedj/cues.sqlite`). Writes still queued
    /// for the drive are dropped, and one in progress is waited for (bounded)
    /// before the delete. Returns false only when a database exists but could
    /// not be deleted; a drive without one counts as success.
    static bool clearFilesystemOverrides(const QString& mountPoint);

    /// Forget what this store knows about the drive mounted at `mountPoint`,
    /// for the eject: a stick that comes back may have been edited elsewhere,
    /// so its next access reads it again. Writes still queued for it become
    /// no-ops, so call this only once they have been flushed. The baseline of
    /// every track whose write is lost that way is marked unsaved, so the next
    /// save of that track stores its cues again even if they have not changed
    /// since: without that a stick yanked and remounted at the same path
    /// would never get them.
    static void forgetFilesystem(const QString& mountPoint);

    /// For tests only: whether this store currently holds a mirror of the drive
    /// mounted at `mountPoint`. Never loads one.
    static bool hasMirrorForTesting(const QString& mountPoint);

    /// Stop flushIfChanged() from re-creating an override for a track that is
    /// still loaded (typically one in a deck) after the overrides were cleared.
    /// The next load of the track re-baselines it and saves again.
    static void suppressPendingSaves();

    /// Locations of the tracks this store has actually put an override on
    /// since startup. Paired with the global track cache to reach the ones
    /// that are still in a deck when the overrides are cleared.
    ///
    /// Deliberately *not* every track it has baselined: a track the drive held
    /// no override for carries nothing of this unit's, so clearing has nothing
    /// to take off it and must not touch it at all.
    static QStringList overriddenLocations();

    /// Put `pTrack` back to the cues its source library exported — the state
    /// captured by applyOverrides() before this unit's override went on top.
    /// Returns false, changing nothing, for a track that never had one.
    ///
    /// This is what makes Settings → Clear take off the DJ's own edits without
    /// taking the rekordbox ANLZ / Serato marker cues with them. Removing every
    /// managed cue instead would blank the pads of a loaded track down to the
    /// imported cues it never owned — and for a Serato track that is permanent,
    /// since markers are imported from the file's tags once and the library
    /// database is authoritative from then on.
    ///
    /// Cues are updated in place wherever a slot survives, for the same reason
    /// applyOverrides() does it: this runs on a track a deck may be playing.
    static bool restoreImportedCues(Track* pTrack);

    /// The stored payload: the track's hot cues, memory cues and main cue as a
    /// compact JSON array with positions in seconds. Also serves as the
    /// baseline to compare a later cue set against, so the same cues always
    /// have to serialize to the same bytes.
    ///
    /// Public so the codec can be exercised without a removable drive under
    /// the test; the store itself only ever reaches it through the calls above.
    static QByteArray serializeCues(const Track& track);

    /// Replace the track's hot cue bank, memory cue bank and main cue with the
    /// ones in `payload`, leaving every other cue (intro, outro, the
    /// analyzer's own) alone.
    static void applyPayload(Track* pTrack, const QByteArray& payload);

  private:
    /// Every override stored on one drive, as loaded from it and updated by
    /// this unit's saves since.
    struct Mirror {
        /// Relative path -> stored payload.
        QHash<QString, QByteArray> payloads;
        /// Relative paths whose payload has been queued but not yet written,
        /// or whose write failed and waits for the next save to retry it.
        QSet<QString> dirty;
        /// Relative path -> the track location this unit queued its last
        /// write under, which is the key its baseline has in s_baselines.
        /// Recorded rather than rebuilt from the mount root, since a location
        /// need not be spelled the way the mount root and relPath join up.
        QHash<QString, QString> locations;
        /// False while the drive's store could not be read: the mirror then
        /// holds only this unit's own writes, and every read of a path it does
        /// not hold tries the drive again (once the retry backoff is over).
        bool complete = false;
    };

    /// What readOverride() found out about a track's stored override.
    enum class StoreRead {
        /// The track's filesystem is unavailable, or not a removable one.
        Unavailable,
        /// The drive's store is there but could not be read (just now, or
        /// recently enough that it was not tried again), and this unit has
        /// written nothing for the track to it either. Says nothing about
        /// whether the drive holds an override for it.
        Unreadable,
        /// The drive holds no override for the track; also the answer for a
        /// drive with no store at all.
        Absent,
        /// The drive holds an override for the track, in `pPayload`.
        Found,
    };

    // Answered from the mirror, which is loaded first if need be.
    static StoreRead readOverride(const QString& trackLocation, QByteArray* pPayload);
    // Make sure the mirror of the drive `target` is on has been loaded (or
    // completed) from the drive. The read itself runs with s_baselineMutex let
    // go, so it must not be held on entry. When the store is there but cannot
    // be read, nothing is loaded and the failure is remembered for
    // kFsStoreLoadRetryMillis, during which this returns without touching the
    // drive; the caller then finds the incomplete mirror of this unit's own
    // writes if there is one, and no mirror otherwise.
    static void ensureLoaded(const FsStoreTarget& target);
    // Drop the mirror of the drive keyed `rootKey` and everything remembered
    // about loading it, so that a read of it in flight is not kept either.
    // Requires s_baselineMutex to be held.
    static void dropMirror(const QString& rootKey);
    // Writer thread: take the payload queued for `relPath` on the drive keyed
    // `rootKey`, if it is still waiting to be written. False when an earlier
    // task already wrote it, or the drive's mirror has been dropped.
    static bool takePendingWrite(
            const QString& rootKey, const QString& relPath, QByteArray* pPayload);
    // Writer thread: after a failed write, mark `relPath` unsaved again so the
    // drive's mirror keeps showing it and forgetFilesystem() reports losing
    // it. Leaves the payload alone, which a newer save may have replaced.
    static void markUnsaved(const QString& rootKey, const QString& relPath);
    // Writer thread: store `payload` for the track at `trackLocation`, provided
    // it still resolves to the drive keyed `rootKey` and that drive's mirror
    // has not been dropped (by a clear or an eject) in the meantime.
    static FsStoreWriteResult writeOverride(const QString& trackLocation,
            const QString& rootKey,
            const QByteArray& payload);

    static QMutex s_baselineMutex;
    // Maps a track's location to the cue set it was loaded with (or last saved
    // with), to a suppression marker set by suppressPendingSaves(), or to the
    // unsaved marker forgetFilesystem() sets.
    static QHash<QString, QByteArray> s_baselines;
    // Maps a track's location to the cue set its source library exported, as it
    // stood just before an override was applied over it. Only holds the tracks
    // that actually got one, which is what restoreImportedCues() keys off.
    static QHash<QString, QByteArray> s_importedCues;
    // Maps a drive's cleaned mount root to its mirror. Guarded by
    // s_baselineMutex, which is never held across disk I/O, not even the
    // read that loads a mirror.
    static QHash<QString, Mirror> s_mirrors;
    // Maps a drive's cleaned mount root to when its store may next be read,
    // after a read that failed. No entry once a read succeeds; an expired one
    // stays until then, which keeps the warning to one per drive.
    static QHash<QString, QDeadlineTimer> s_loadRetryAfter;
    // Maps a drive's cleaned mount root to how many times its mirror has been
    // dropped, so that a read which started before a drop is not merged into
    // a mirror loaded after it.
    static QHash<QString, quint64> s_dropGenerations;
};
