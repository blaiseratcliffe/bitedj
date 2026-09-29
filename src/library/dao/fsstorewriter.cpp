#include "library/dao/fsstorewriter.h"

#include <QDir>
#include <QMutexLocker>
#include <QThread>
#include <QtDebug>
#include <utility>

#include "moc_fsstorewriter.cpp"
#include "util/assert.h"
#include "util/rtscheduling.h"

namespace {

const char kLogTag[] = "FsStoreWriter";

/// Pending counts are keyed by the cleaned mount root, so "/media/x/" and
/// "/media/x" are the same drive.
QString keyForMount(const QString& mountRoot) {
    return QDir::cleanPath(mountRoot);
}

} // anonymous namespace

// static
QMutex FsStoreWriter::s_instanceMutex;
// static
FsStoreWriter* FsStoreWriter::s_pInstance = nullptr;

FsStoreWriter::FsStoreWriter(QObject* parent)
        : QObject(parent),
          m_pThread(new QThread(this)),
          m_pWorkerContext(new QObject()) {
    m_pThread->setObjectName(QStringLiteral("FsStoreWriter"));
    m_pWorkerContext->moveToThread(m_pThread);
    connect(m_pThread, &QThread::finished, m_pWorkerContext, &QObject::deleteLater);
    connect(m_pThread, &QThread::started, m_pWorkerContext, []() {
        // A store write records something the DJ already sees (a cue on the
        // pad, a star in the view, a sample in the slot), and it spends its
        // time blocked on a USB stick. Demoted from inside the thread for the
        // same reasons FsHistoryWorker gives: the thread is created by the
        // SCHED_FIFO GUI thread, and only this call also lowers the I/O
        // priority, which keeps the write from queueing in front of a deck
        // reading the track it is about to play.
        mixxx::demoteCurrentThreadToBackground(kLogTag);
    });
    m_pThread->start(QThread::LowPriority);

    QMutexLocker locker(&s_instanceMutex);
    VERIFY_OR_DEBUG_ASSERT(!s_pInstance) {
        // Two writers would split one drive's writes over two queues, and the
        // eject would only wait for one of them. Keep the first.
        return;
    }
    s_pInstance = this;
}

FsStoreWriter::~FsStoreWriter() {
    {
        QMutexLocker locker(&s_instanceMutex);
        if (s_pInstance == this) {
            s_pInstance = nullptr;
        }
    }
    // Unregistered first: from here on a save writes on its caller's thread,
    // so nothing new joins the queue while it drains. The drain is bounded
    // because a dead stick must not hang power-off; quit() below drops
    // whatever is still posted, which is harmless since a task that never runs
    // never touches this object. A task that is running is not harmless, which
    // is why wait() is unbounded.
    if (!waitForPending(QString(), QDeadlineTimer(kFlushTimeoutMillis))) {
        QMutexLocker locker(&m_pendingMutex);
        int pending = 0;
        for (auto it = m_pendingByMount.constBegin(); it != m_pendingByMount.constEnd(); ++it) {
            pending += it.value();
        }
        qWarning() << kLogTag << ": shutting down with" << pending
                   << "store write(s) not done after" << kFlushTimeoutMillis
                   << "ms; dropping the ones still queued:" << m_pendingByMount;
    }
    m_pThread->quit();
    m_pThread->wait();
}

void FsStoreWriter::enqueue(const QString& mountKey, std::function<void()> task) {
    {
        QMutexLocker locker(&m_pendingMutex);
        ++m_pendingByMount[mountKey];
    }
    QMetaObject::invokeMethod(
            m_pWorkerContext,
            [this, mountKey, task = std::move(task)]() {
                task();
                {
                    QMutexLocker locker(&m_pendingMutex);
                    const auto it = m_pendingByMount.find(mountKey);
                    if (it != m_pendingByMount.end() && --it.value() <= 0) {
                        m_pendingByMount.erase(it);
                    }
                }
                m_pendingDone.wakeAll();
            },
            Qt::QueuedConnection);
}

bool FsStoreWriter::waitForPending(const QString& mountKey, QDeadlineTimer deadline) {
    QMutexLocker locker(&m_pendingMutex);
    const auto isPending = [this, &mountKey]() {
        return mountKey.isEmpty() ? !m_pendingByMount.isEmpty()
                                  : m_pendingByMount.contains(mountKey);
    };
    while (isPending()) {
        if (!m_pendingDone.wait(&m_pendingMutex, deadline)) {
            return !isPending();
        }
    }
    return true;
}

// static
void FsStoreWriter::submit(const QString& mountRoot, std::function<void()> task) {
    {
        QMutexLocker locker(&s_instanceMutex);
        if (s_pInstance) {
            s_pInstance->enqueue(keyForMount(mountRoot), std::move(task));
            return;
        }
    }
    // No writer: run it here, as every store did before there was one. Outside
    // the registry lock, which the task has no business holding.
    task();
}

// static
bool FsStoreWriter::flushFilesystem(const QString& mountRoot, QDeadlineTimer deadline) {
    const QString mountKey = keyForMount(mountRoot);
    if (mountKey.isEmpty()) {
        // Nothing is ever queued without a drive, and an empty key would mean
        // "every drive" to waitForPending().
        return true;
    }
    // Held across the wait so the writer cannot be destroyed under us; its
    // destructor takes the same mutex first, and the writer thread never
    // takes it at all, so this cannot deadlock.
    QMutexLocker locker(&s_instanceMutex);
    if (!s_pInstance) {
        return true;
    }
    if (s_pInstance->waitForPending(mountKey, deadline)) {
        return true;
    }
    // One queue serves every drive, so what is still pending may be another
    // drive's writes ahead of this one's; the drive named is the one waited
    // for.
    qWarning() << kLogTag << ": waiting for store writes to" << mountRoot
               << "ran out of time with writes still queued for it or ahead of "
                  "it; carrying on without them";
    return false;
}

// static
bool FsStoreWriter::flushAll(QDeadlineTimer deadline) {
    QMutexLocker locker(&s_instanceMutex);
    if (!s_pInstance) {
        return true;
    }
    if (s_pInstance->waitForPending(QString(), deadline)) {
        return true;
    }
    qWarning() << kLogTag
               << ": store writes are still pending at the deadline; carrying on "
                  "without them";
    return false;
}
