#include "library/dao/fsstorewriter.h"

#include <gtest/gtest.h>

#include <QAtomicInt>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QPair>
#include <QSemaphore>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStorageInfo>
#include <QThread>
#include <algorithm>
#include <memory>
#include <thread>
#include <utility>

#include "library/dao/fscueoverridestore.h"
#include "library/dao/fsmetaoverridestore.h"
#include "test/mixxxtest.h"
#include "track/cue.h"
#include "track/track.h"

namespace {

// Like FsCueOverrideStoreTest, these need a filesystem mounted under one of the
// removable roots, which `unshare` provides without root:
//
//   unshare -Umr --propagation private sh -c \
//     'mount -t tmpfs tmpfs /mnt && mkdir -p /mnt/usbtest && \
//      mount -t tmpfs tmpfs /mnt/usbtest && \
//      QT_QPA_PLATFORM=offscreen ./mixxx-test \
//        --gtest_filter="FsStoreWriterTest.*"'
//
// The cases skip when that mount is not there, so an ordinary run stays green.
const QString kFakeUsb = QStringLiteral("/mnt/usbtest");

constexpr auto kSampleRate = mixxx::audio::SampleRate(44100);

QDeadlineTimer flushDeadline() {
    return QDeadlineTimer(FsStoreWriter::kFlushTimeoutMillis);
}

/// The cue payload stored for `relPath` in the database at `dbPath`, read with
/// a connection of its own so that neither the store nor its mirror has any
/// say in the answer. Empty when there is no database or no entry. Safe to
/// call from the writer thread.
QByteArray readStoredCues(const QString& dbPath, const QString& relPath) {
    if (!QFileInfo::exists(dbPath)) {
        // Opening it would create it.
        return QByteArray();
    }
    static QAtomicInt counter;
    const QString connectionName =
            QStringLiteral("fsstorewriter-test-%1").arg(counter.fetchAndAddRelaxed(1));
    QByteArray payload;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        db.setDatabaseName(dbPath);
        if (db.open()) {
            QSqlQuery query(db);
            query.prepare(QStringLiteral(
                    "SELECT cues FROM cue_overrides WHERE relpath = :relpath"));
            query.bindValue(QStringLiteral(":relpath"), relPath);
            if (query.exec() && query.next()) {
                payload = query.value(0).toString().toUtf8();
            }
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(connectionName);
    return payload;
}

/// Covers what moving the .bitedj store writes onto FsStoreWriter changes: a
/// save returns before the drive has it, reads see it anyway, saves that pile
/// up behind a slow write collapse into one, and a drive that is cleared or
/// forgotten does not get a queued write afterwards.
///
/// "Slow" is simulated by parking the writer thread on a task that waits for
/// the test to let it go, which is exactly what a stick taking seconds over a
/// write looks like from the GUI thread.
class FsStoreWriterTest : public MixxxTest {
  protected:
    void SetUp() override {
        MixxxTest::SetUp();
        if (!haveFakeUsb()) {
            return;
        }
        // Cleared before the writer exists, so this happens right here.
        ASSERT_TRUE(FsCueOverrideStore::clearFilesystemOverrides(kFakeUsb));
        ASSERT_TRUE(FsMetaOverrideStore::clearFilesystemOverrides(kFakeUsb));
        m_pWriter = std::make_unique<FsStoreWriter>();
    }

    void TearDown() override {
        // Never leave the writer parked: its destructor would sit out the
        // whole shutdown drain.
        releaseWriter();
        m_pWriter.reset();
        if (haveFakeUsb()) {
            for (const QString& path : std::as_const(m_placedTracks)) {
                QFile::remove(path);
            }
            FsCueOverrideStore::clearFilesystemOverrides(kFakeUsb);
            FsMetaOverrideStore::clearFilesystemOverrides(kFakeUsb);
        }
        MixxxTest::TearDown();
    }

    static bool haveFakeUsb() {
        const QStorageInfo usb(kFakeUsb);
        return usb.isValid() && usb.isReady() && usb.rootPath() == kFakeUsb;
    }

    static QString cueDbPath() {
        return kFakeUsb + QStringLiteral("/.bitedj/cues.sqlite");
    }

    static QString metaDbPath() {
        return kFakeUsb + QStringLiteral("/.bitedj/meta.sqlite");
    }

    /// Put a track file on the pretend drive; the stores only resolve files
    /// that exist.
    QString placeTrack(const QString& fileName) {
        const QString path = kFakeUsb + QLatin1Char('/') + fileName;
        QFile::remove(path);
        EXPECT_TRUE(QFile::copy(getTestDir().filePath(QStringLiteral("sine-30.wav")), path));
        m_placedTracks.append(path);
        return path;
    }

    static TrackPointer makeTrack(const QString& path) {
        auto pTrack = Track::newTemporary(mixxx::FileAccess(mixxx::FileInfo(path)));
        pTrack->setAudioProperties(mixxx::audio::ChannelCount(2),
                kSampleRate,
                mixxx::audio::Bitrate(),
                mixxx::Duration::fromSeconds(180));
        return pTrack;
    }

    static void addHotcue(const TrackPointer& pTrack, int slot, double seconds) {
        pTrack->createAndAddCue(mixxx::CueType::HotCue,
                slot,
                mixxx::audio::FramePos(seconds * kSampleRate),
                mixxx::audio::kInvalidFramePos);
    }

    // By slot: Track::findCueByType() does not find hot cues.
    static CuePointer findHotcue(const TrackPointer& pTrack, int hotcueIndex) {
        const QList<CuePointer> cues = pTrack->getCuePoints();
        for (const CuePointer& pCue : cues) {
            if (pCue->getHotCue() == hotcueIndex) {
                return pCue;
            }
        }
        return CuePointer();
    }

    static QList<int> hotcueIndices(const TrackPointer& pTrack) {
        QList<int> indices;
        const QList<CuePointer> cues = pTrack->getCuePoints();
        for (const CuePointer& pCue : cues) {
            if (pCue->getHotCue() != Cue::kNoHotCue) {
                indices << pCue->getHotCue();
            }
        }
        std::sort(indices.begin(), indices.end());
        return indices;
    }

    /// Park the writer thread until releaseWriter(). Returns once the parking
    /// task is running, so everything submitted after this queues behind it.
    void blockWriter() {
        ASSERT_FALSE(m_blocked);
        FsStoreWriter::submit(kFakeUsb, [this]() {
            m_parked.release();
            m_gate.acquire();
        });
        m_parked.acquire();
        m_blocked = true;
    }

    void releaseWriter() {
        if (m_blocked) {
            m_blocked = false;
            m_gate.release();
        }
    }

    std::unique_ptr<FsStoreWriter> m_pWriter;
    QSemaphore m_parked;
    QSemaphore m_gate;
    bool m_blocked = false;
    QStringList m_placedTracks;
};

#define SKIP_WITHOUT_FAKE_USB()                                                   \
    if (!haveFakeUsb()) {                                                         \
        GTEST_SKIP() << "needs a filesystem mounted at " << qPrintable(kFakeUsb); \
    }

} // anonymous namespace

// The point of the change: a save returns while the drive is still busy, and
// the edit is not lost in the meantime, because a load reads what was saved
// rather than what has reached the stick.
TEST_F(FsStoreWriterTest, ReadYourWritesWhileQueued) {
    SKIP_WITHOUT_FAKE_USB();
    const QString trackPath = placeTrack(QStringLiteral("queued.wav"));

    const TrackPointer pFirst = makeTrack(trackPath);
    FsCueOverrideStore::applyOverrides(pFirst.get());

    blockWriter();
    addHotcue(pFirst, mixxx::kHotCueBankStart + 2, 11.0);
    FsCueOverrideStore::flushIfChanged(*pFirst);
    // Returned, and nothing is on the drive yet.
    EXPECT_FALSE(QFile::exists(cueDbPath()));

    // A load in the meantime still gets the edit.
    const TrackPointer pSecond = makeTrack(trackPath);
    FsCueOverrideStore::applyOverrides(pSecond.get());
    EXPECT_EQ(QList<int>({mixxx::kHotCueBankStart + 2}), hotcueIndices(pSecond));

    releaseWriter();
    ASSERT_TRUE(FsStoreWriter::flushAll(flushDeadline()));
    EXPECT_TRUE(QFile::exists(cueDbPath()));

    // And it is really on the drive: with the store's copy forgotten, a load
    // has only the file to go on.
    FsCueOverrideStore::forgetFilesystem(kFakeUsb);
    const TrackPointer pThird = makeTrack(trackPath);
    FsCueOverrideStore::applyOverrides(pThird.get());
    EXPECT_EQ(QList<int>({mixxx::kHotCueBankStart + 2}), hotcueIndices(pThird));
}

// Saves that pile up behind a slow write collapse: the first queued write
// stores what the track holds by the time it runs, not what it held when it
// was queued, and the ones behind it have nothing left to do.
TEST_F(FsStoreWriterTest, CoalescesRepeatedSaves) {
    SKIP_WITHOUT_FAKE_USB();
    const QString trackPath = placeTrack(QStringLiteral("coalesce.wav"));
    const QString relPath = QStringLiteral("coalesce.wav");

    const TrackPointer pTrack = makeTrack(trackPath);
    FsCueOverrideStore::applyOverrides(pTrack.get());

    blockWriter();
    addHotcue(pTrack, mixxx::kHotCueBankStart, 1.0);
    FsCueOverrideStore::flushIfChanged(*pTrack);
    // Runs right after the first queued write and before the other two, and
    // notes what the drive holds at that point.
    QByteArray storedAfterFirstWrite;
    FsStoreWriter::submit(kFakeUsb, [&storedAfterFirstWrite, relPath]() {
        storedAfterFirstWrite = readStoredCues(cueDbPath(), relPath);
    });
    addHotcue(pTrack, mixxx::kHotCueBankStart + 1, 2.0);
    FsCueOverrideStore::flushIfChanged(*pTrack);
    const CuePointer pFirstPad = findHotcue(pTrack, mixxx::kHotCueBankStart);
    ASSERT_TRUE(pFirstPad);
    pFirstPad->setStartAndEndPosition(mixxx::audio::FramePos(5.0 * kSampleRate),
            mixxx::audio::kInvalidFramePos);
    FsCueOverrideStore::flushIfChanged(*pTrack);
    const QByteArray last = FsCueOverrideStore::serializeCues(*pTrack);

    releaseWriter();
    ASSERT_TRUE(FsStoreWriter::flushAll(flushDeadline()));

    // The first write already carried the third save.
    EXPECT_EQ(last, storedAfterFirstWrite);
    EXPECT_EQ(last, readStoredCues(cueDbPath(), relPath));

    FsCueOverrideStore::forgetFilesystem(kFakeUsb);
    const TrackPointer pFresh = makeTrack(trackPath);
    FsCueOverrideStore::applyOverrides(pFresh.get());
    EXPECT_EQ(last, FsCueOverrideStore::serializeCues(*pFresh));
}

// A drive the store has forgotten (the eject does this) takes no write that
// was still queued for it.
TEST_F(FsStoreWriterTest, ForgottenDriveTakesNoQueuedWrite) {
    SKIP_WITHOUT_FAKE_USB();
    const QString trackPath = placeTrack(QStringLiteral("forgotten.wav"));

    const TrackPointer pTrack = makeTrack(trackPath);
    FsCueOverrideStore::applyOverrides(pTrack.get());

    blockWriter();
    addHotcue(pTrack, mixxx::kHotCueBankStart, 3.0);
    FsCueOverrideStore::flushIfChanged(*pTrack);
    FsCueOverrideStore::forgetFilesystem(kFakeUsb);
    releaseWriter();
    ASSERT_TRUE(FsStoreWriter::flushAll(flushDeadline()));

    EXPECT_FALSE(QFile::exists(cueDbPath()));
}

// Clearing a drive while a write for it is queued: the write does not land,
// before the delete or after it.
TEST_F(FsStoreWriterTest, ClearDropsQueuedWrite) {
    SKIP_WITHOUT_FAKE_USB();
    const QString trackPath = placeTrack(QStringLiteral("cleared.wav"));

    const TrackPointer pTrack = makeTrack(trackPath);
    FsCueOverrideStore::applyOverrides(pTrack.get());

    blockWriter();
    addHotcue(pTrack, mixxx::kHotCueBankStart, 4.0);
    FsCueOverrideStore::flushIfChanged(*pTrack);
    // Runs right after the queued write, and before the clear's delete (the
    // clear waits for everything queued for the drive), so it sees whether
    // that write was dropped or merely deleted again afterwards.
    bool dbExistedAfterQueuedWrite = true;
    FsStoreWriter::submit(kFakeUsb, [&dbExistedAfterQueuedWrite]() {
        dbExistedAfterQueuedWrite = QFile::exists(cueDbPath());
    });

    ASSERT_TRUE(FsCueOverrideStore::hasMirrorForTesting(kFakeUsb));

    // The clear waits for the writer, which waits for this thread to let it
    // go, so it runs on a thread of its own.
    bool cleared = false;
    std::thread clearThread([&cleared]() {
        cleared = FsCueOverrideStore::clearFilesystemOverrides(kFakeUsb);
    });
    // The writer is released only once the clear has dropped the drive's
    // mirror, and that is necessarily before its delete: the delete waits for
    // everything queued for the drive, and the parked writer holds all of it
    // up. So the queued write runs between the drop and the delete. Honouring
    // the drop, it writes nothing (the probe sees no database); ignoring it,
    // it writes, which the probe sees. A clear that did not wait at all would
    // delete first and leave the late write to recreate the database, which
    // the check at the end sees.
    QElapsedTimer waited;
    waited.start();
    while (FsCueOverrideStore::hasMirrorForTesting(kFakeUsb) && waited.elapsed() < 5000) {
        QThread::msleep(1);
    }
    const bool mirrorDropped = !FsCueOverrideStore::hasMirrorForTesting(kFakeUsb);
    releaseWriter();
    clearThread.join();
    ASSERT_TRUE(mirrorDropped) << "the clear never dropped the drive's mirror";

    EXPECT_TRUE(cleared);
    ASSERT_TRUE(FsStoreWriter::flushAll(flushDeadline()));
    EXPECT_FALSE(dbExistedAfterQueuedWrite);
    EXPECT_FALSE(QFile::exists(cueDbPath()));

    // And the store does not remember the dropped edit either.
    const TrackPointer pFresh = makeTrack(trackPath);
    FsCueOverrideStore::applyOverrides(pFresh.get());
    EXPECT_TRUE(hotcueIndices(pFresh).isEmpty());
}

// A rating edited in a Rekordbox view is accepted, read back and announced at
// once, so the view repaints without waiting for the drive, which gets it
// later.
TEST_F(FsStoreWriterTest, MetaStoreRatingIsAsyncAndSignals) {
    SKIP_WITHOUT_FAKE_USB();
    const QString trackPath = placeTrack(QStringLiteral("rated.wav"));

    // Touched here first, on the GUI thread, as RekordboxFeature does.
    FsMetaOverrideNotifier& notifier = FsMetaOverrideNotifier::instance();
    QObject receiver;
    QList<QPair<QString, int>> stored;
    QObject::connect(&notifier,
            &FsMetaOverrideNotifier::ratingStored,
            &receiver,
            [&stored](const QString& trackLocation, int rating) {
                stored.append(qMakePair(trackLocation, rating));
            });

    blockWriter();
    ASSERT_TRUE(FsMetaOverrideStore::storeRating(trackPath, 4));
    EXPECT_FALSE(QFile::exists(metaDbPath()));
    // Announced before storeRating() returned, on this thread, while the
    // drive has nothing yet.
    ASSERT_EQ(1, stored.size());
    EXPECT_EQ(trackPath, stored.first().first);
    EXPECT_EQ(4, stored.first().second);
    // The rekordbox scan sees it before the drive does.
    EXPECT_EQ(4, FsMetaOverrideStore::readMountRatings(kFakeUsb).ratingFor(trackPath, 0));

    releaseWriter();
    ASSERT_TRUE(FsStoreWriter::flushAll(flushDeadline()));
    EXPECT_TRUE(QFile::exists(metaDbPath()));
    // The write landing is not announced a second time.
    QCoreApplication::processEvents();
    EXPECT_EQ(1, stored.size());

    // And it is really on the drive.
    FsMetaOverrideStore::forgetFilesystem(kFakeUsb);
    EXPECT_EQ(4, FsMetaOverrideStore::readMountRatings(kFakeUsb).ratingFor(trackPath, 0));
}

// Without a writer (the store tests, and anything saved before the
// TrackCollectionManager exists or after it is gone) a save is written before
// it returns, as it always was.
TEST_F(FsStoreWriterTest, NoInstanceWritesSynchronously) {
    SKIP_WITHOUT_FAKE_USB();
    m_pWriter.reset();
    const QString trackPath = placeTrack(QStringLiteral("direct.wav"));

    const TrackPointer pTrack = makeTrack(trackPath);
    FsCueOverrideStore::applyOverrides(pTrack.get());
    addHotcue(pTrack, mixxx::kHotCueBankStart + 6, 7.0);
    FsCueOverrideStore::flushIfChanged(*pTrack);

    EXPECT_EQ(FsCueOverrideStore::serializeCues(*pTrack),
            readStoredCues(cueDbPath(), QStringLiteral("direct.wav")));
}
