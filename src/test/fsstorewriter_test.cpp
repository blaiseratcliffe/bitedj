#include "library/dao/fsstorewriter.h"

#include <gtest/gtest.h>

#include <QAtomicInt>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
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

/// Create at `dbPath` a cue store that the store cannot read but would still
/// write to, holding an override for `relPath`. `cue_overrides` is a view
/// whose every row fails to evaluate (abs() of the smallest integer is an
/// overflow error), so the store's SELECT fails the way it would on a store
/// written by a future schema. An INSTEAD OF trigger passes inserts through
/// to the table underneath, so a blind INSERT OR REPLACE lands and changes
/// the file, which is what the test watches for.
bool createUnreadableCueStore(const QString& dbPath, const QString& relPath) {
    static QAtomicInt counter;
    const QString connectionName =
            QStringLiteral("fsstorewriter-unreadable-%1").arg(counter.fetchAndAddRelaxed(1));
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        db.setDatabaseName(dbPath);
        if (db.open()) {
            QSqlQuery query(db);
            ok = query.exec(QStringLiteral(
                         "CREATE TABLE stored (relpath TEXT PRIMARY KEY NOT NULL, "
                         "version INTEGER NOT NULL, updated_at TEXT, cues TEXT NOT NULL)")) &&
                    query.exec(QStringLiteral(
                            "CREATE VIEW cue_overrides AS SELECT relpath, version, "
                            "updated_at, cues || abs(-9223372036854775808) AS cues "
                            "FROM stored")) &&
                    query.exec(QStringLiteral(
                            "CREATE TRIGGER cue_overrides_insert INSTEAD OF INSERT "
                            "ON cue_overrides BEGIN INSERT OR REPLACE INTO stored "
                            "VALUES (NEW.relpath, NEW.version, NEW.updated_at, "
                            "NEW.cues); END"));
            if (ok) {
                query.prepare(QStringLiteral(
                        "INSERT INTO stored VALUES (:relpath, 1, NULL, :cues)"));
                query.bindValue(QStringLiteral(":relpath"), relPath);
                query.bindValue(QStringLiteral(":cues"),
                        QStringLiteral("[{\"color\":0,\"pos\":1.5,\"slot\":0,\"type\":1}]"));
                ok = query.exec();
            }
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(connectionName);
    return ok;
}

QByteArray fileBytes(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
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

// A stick that drops off the bus while a save for it is queued loses that
// save with its mirror. The save already moved the baseline to the new cues,
// so unless forgetting the drive undoes that, the next save of the same cues
// is skipped as unchanged and the stick, remounted at the same path, never
// gets them.
TEST_F(FsStoreWriterTest, CuesLostWithForgottenDriveAreSavedAgain) {
    SKIP_WITHOUT_FAKE_USB();
    const QString trackPath = placeTrack(QStringLiteral("yanked.wav"));
    const QString relPath = QStringLiteral("yanked.wav");

    const TrackPointer pTrack = makeTrack(trackPath);
    FsCueOverrideStore::applyOverrides(pTrack.get());

    blockWriter();
    addHotcue(pTrack, mixxx::kHotCueBankStart + 3, 9.0);
    FsCueOverrideStore::flushIfChanged(*pTrack);
    // What SystemSettings::refresh() does when the stick vanishes.
    FsCueOverrideStore::forgetFilesystem(kFakeUsb);
    releaseWriter();
    ASSERT_TRUE(FsStoreWriter::flushAll(flushDeadline()));
    ASSERT_TRUE(readStoredCues(cueDbPath(), relPath).isEmpty());

    // The stick is back at the same path. The track is loaded again (a
    // Rekordbox playlist does this to a track still in a deck), which must
    // not make the lost cues look stored, and then saved with the cues it
    // had.
    FsCueOverrideStore::applyOverrides(pTrack.get());
    FsCueOverrideStore::flushIfChanged(*pTrack);
    ASSERT_TRUE(FsStoreWriter::flushAll(flushDeadline()));
    EXPECT_EQ(FsCueOverrideStore::serializeCues(*pTrack),
            readStoredCues(cueDbPath(), relPath));
}

// The same for a save that deleted every cue. It is the case that rules out
// simply dropping the baseline: a track with no baseline and no cues is not
// saved at all, so the drive would keep the cues the DJ deleted.
TEST_F(FsStoreWriterTest, DeletionLostWithForgottenDriveIsSavedAgain) {
    SKIP_WITHOUT_FAKE_USB();
    const QString trackPath = placeTrack(QStringLiteral("yanked-deleted.wav"));
    const QString relPath = QStringLiteral("yanked-deleted.wav");

    const TrackPointer pTrack = makeTrack(trackPath);
    FsCueOverrideStore::applyOverrides(pTrack.get());
    addHotcue(pTrack, mixxx::kHotCueBankStart + 4, 12.0);
    FsCueOverrideStore::flushIfChanged(*pTrack);
    ASSERT_TRUE(FsStoreWriter::flushAll(flushDeadline()));
    const QByteArray withCue = FsCueOverrideStore::serializeCues(*pTrack);
    ASSERT_EQ(withCue, readStoredCues(cueDbPath(), relPath));

    blockWriter();
    const CuePointer pCue = findHotcue(pTrack, mixxx::kHotCueBankStart + 4);
    ASSERT_TRUE(pCue);
    pTrack->removeCue(pCue);
    const QByteArray noCues = FsCueOverrideStore::serializeCues(*pTrack);
    FsCueOverrideStore::flushIfChanged(*pTrack);
    FsCueOverrideStore::forgetFilesystem(kFakeUsb);
    releaseWriter();
    ASSERT_TRUE(FsStoreWriter::flushAll(flushDeadline()));
    ASSERT_EQ(withCue, readStoredCues(cueDbPath(), relPath));

    FsCueOverrideStore::flushIfChanged(*pTrack);
    ASSERT_TRUE(FsStoreWriter::flushAll(flushDeadline()));
    EXPECT_EQ(noCues, readStoredCues(cueDbPath(), relPath));
}

// A track loaded while its drive's store is there but cannot be read came
// without whatever override the drive holds for it, so an edit to it must not
// be saved: that would replace the override with the imported cues plus the
// edit. The store file is left exactly as it was.
TEST_F(FsStoreWriterTest, UnreadableStorePausesSaves) {
    SKIP_WITHOUT_FAKE_USB();
    const QString trackPath = placeTrack(QStringLiteral("unreadable.wav"));
    ASSERT_TRUE(QDir().mkpath(kFakeUsb + QStringLiteral("/.bitedj")));
    ASSERT_TRUE(createUnreadableCueStore(cueDbPath(), QStringLiteral("unreadable.wav")));
    const QByteArray before = fileBytes(cueDbPath());
    ASSERT_FALSE(before.isEmpty());

    const TrackPointer pTrack = makeTrack(trackPath);
    FsCueOverrideStore::applyOverrides(pTrack.get());
    // Nothing could be read, so nothing was applied.
    EXPECT_TRUE(hotcueIndices(pTrack).isEmpty());

    addHotcue(pTrack, mixxx::kHotCueBankStart + 5, 6.0);
    FsCueOverrideStore::flushIfChanged(*pTrack);
    ASSERT_TRUE(FsStoreWriter::flushAll(flushDeadline()));
    EXPECT_EQ(before, fileBytes(cueDbPath()));
}

// A rating saved off a deck and lost with its drive's mirror is saved again,
// as the cues are above, even after the track is loaded again.
TEST_F(FsStoreWriterTest, RatingLostWithForgottenDriveIsSavedAgain) {
    SKIP_WITHOUT_FAKE_USB();
    const QString trackPath = placeTrack(QStringLiteral("yanked-rated.wav"));

    const TrackPointer pTrack = makeTrack(trackPath);
    FsMetaOverrideStore::applyOverrides(pTrack.get());

    blockWriter();
    pTrack->setRating(3);
    FsMetaOverrideStore::flushIfChanged(*pTrack);
    FsMetaOverrideStore::forgetFilesystem(kFakeUsb);
    releaseWriter();
    ASSERT_TRUE(FsStoreWriter::flushAll(flushDeadline()));
    ASSERT_FALSE(QFile::exists(metaDbPath()));

    // Loaded again before the save, as in the cue case.
    FsMetaOverrideStore::applyOverrides(pTrack.get());
    FsMetaOverrideStore::flushIfChanged(*pTrack);
    ASSERT_TRUE(FsStoreWriter::flushAll(flushDeadline()));
    // Read with the store's copy forgotten, so only the drive can answer.
    FsMetaOverrideStore::forgetFilesystem(kFakeUsb);
    EXPECT_EQ(3, FsMetaOverrideStore::readMountRatings(kFakeUsb).ratingFor(trackPath, 0));
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
    // Runs right after the queued write, so a write that reached the drive
    // shows here even though the clear's delete would remove it again.
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
    // mirror, so the queued write runs after the drop. Honouring it, the write
    // does nothing (the probe sees no database); ignoring it, the write
    // lands, which the probe sees whether the clear's delete has happened yet
    // or not. What this verifies is that a write queued before a clear does
    // not reach the drive after it. It cannot tell whether the clear waits
    // for a write already in progress before deleting: the mirror is dropped
    // first, so the queued write finds nothing to do either way.
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
