#pragma once

#include <QDeadlineTimer>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QWaitCondition>
#include <functional>

class QThread;

/// The thread every write to a drive's `.bitedj` stores (cues.sqlite,
/// meta.sqlite, samplers.sqlite) is carried out on, so that none of them runs
/// on the GUI thread.
///
/// A slow vfat stick has been measured taking seconds to accept one SQLite
/// write, and these writes used to be made synchronously from a track save, a
/// rating edit or a sampler slot change: the UI and the controller froze for as
/// long as the stick took. The stores now keep an in-memory copy of what each
/// drive holds, answer every read from it, and hand the write itself to this
/// class. See FsCueOverrideStore for how a store uses it.
///
/// Modelled on FsHistoryWorker: one queue for every drive, and a pending count
/// per mount so the eject can wait for the one drive it is pulling. With no
/// writer registered (the store unit tests, and anything that saves before the
/// TrackCollectionManager exists or after it is gone) a submitted task runs on
/// the caller's thread, which is how every store worked before this class.
///
/// A task must never call submit(), flushFilesystem() or flushAll(): a flush
/// holds the registry lock while it waits for the tasks, so a task that needed
/// it would wait for itself. Tasks run with no lock held beyond what they take
/// themselves, and should hold a store's own mutex only around its in-memory
/// state, never across the disk I/O, since the GUI thread reads under it.
///
/// Nothing here ever emits with Qt::BlockingQueuedConnection, and a task must
/// not either: a flush blocks the GUI thread, so a task waiting on it would
/// deadlock the pair of them.
class FsStoreWriter : public QObject {
    Q_OBJECT

  public:
    /// How long the shutdown drain waits, and the budget the eject and the
    /// clear actions give a flush. Generous next to the seconds a slow stick
    /// takes for one write, short enough that a drive that has stopped
    /// answering does not hold the GUI (or power-off) forever.
    static constexpr int kFlushTimeoutMillis = 10000;

    /// Starts the thread and registers this as the single writer the static
    /// calls below reach.
    explicit FsStoreWriter(QObject* parent = nullptr);
    /// Unregisters first, so from then on stores write on their callers'
    /// threads, then drains what is queued for at most kFlushTimeoutMillis.
    /// Whatever is still queued after that is dropped with a warning; a task
    /// that is already running is always waited out.
    ~FsStoreWriter() override;

    /// Queue `task` to run on the writer thread, counted as pending against
    /// the drive mounted at `mountRoot` until it has run. With no writer
    /// registered the task runs right here, before this returns. Safe to call
    /// from any thread, but not from a task, and not while holding a lock the
    /// task takes.
    static void submit(const QString& mountRoot, std::function<void()> task);

    /// Block until nothing is pending for the drive mounted at `mountRoot`, or
    /// until `deadline` passes. There is one queue for every drive, so this
    /// waits out whatever is queued ahead too. Returns whether the drive's
    /// writes all landed; true when there is no writer. Once the deadline has
    /// passed it returns at once.
    static bool flushFilesystem(const QString& mountRoot, QDeadlineTimer deadline);

    /// flushFilesystem() for every drive.
    static bool flushAll(QDeadlineTimer deadline);

  private:
    void enqueue(const QString& mountKey, std::function<void()> task);
    /// Wait until nothing is pending for `mountKey`, or for any mount when it
    /// is empty. Returns false if the deadline passed first.
    bool waitForPending(const QString& mountKey, QDeadlineTimer deadline);

    QThread* m_pThread;
    /// Lives on m_pThread; the object queued tasks are posted to.
    QObject* m_pWorkerContext;

    QMutex m_pendingMutex;
    QWaitCondition m_pendingDone;
    /// Cleaned mount root -> tasks queued for it that have not finished. A
    /// mount with nothing pending has no entry.
    QHash<QString, int> m_pendingByMount;

    /// Guards s_pInstance, and is held across a flush so the writer cannot be
    /// destroyed under it.
    static QMutex s_instanceMutex;
    static FsStoreWriter* s_pInstance;
};
