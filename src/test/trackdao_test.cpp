#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <QDeadlineTimer>
#include <QFile>
#include <QStorageInfo>
#include <vector>

#include "analyzer/analyzersilence.h"
#include "analyzer/analyzertrack.h"
#include "analyzer/constants.h"
#include "library/dao/fscueoverridestore.h"
#include "library/dao/fsstorewriter.h"
#include "test/librarytest.h"
#include "track/cue.h"
#include "track/track.h"

using ::testing::UnorderedElementsAre;

namespace {

// A filesystem mounted under one of the removable roots, for the tests of the
// per-drive cue store. See fscueoverridestore_test.cpp for how to provide one
// without root. The tests that need it skip themselves when it is missing.
const QString kFakeUsb = QStringLiteral("/mnt/usbtest");

bool fakeUsbIsMounted() {
    // An unmounted path reports itself as its own root, so validity is what
    // tells a real mount from a missing one.
    const QStorageInfo usb(kFakeUsb);
    return usb.isValid() && usb.isReady() && usb.rootPath() == kFakeUsb;
}

} // namespace

class TrackDAOTest : public LibraryTest {
  protected:
    // Copy the test tone onto the fake stick as `fileName`, on a drive whose
    // cue store starts out empty.
    QString copyToFakeUsb(const QString& fileName) {
        const QString trackPath = kFakeUsb + QLatin1Char('/') + fileName;
        QFile::remove(trackPath);
        EXPECT_TRUE(FsCueOverrideStore::clearFilesystemOverrides(kFakeUsb));
        EXPECT_TRUE(QFile::copy(
                getTestDir().filePath(QStringLiteral("sine-30.wav")), trackPath));
        return trackPath;
    }

    static QString cueStorePath() {
        return kFakeUsb + QStringLiteral("/.bitedj/cues.sqlite");
    }
};


TEST_F(TrackDAOTest, detectMovedTracks) {
    TrackDAO& trackDAO = internalCollection()->getTrackDAO();

    QString filename = QStringLiteral("file.mp3");

    mixxx::FileInfo oldFile(QDir(QDir::tempPath() + QStringLiteral("/old/dir1")), filename);
    mixxx::FileInfo newFile(QDir(QDir::tempPath() + QStringLiteral("/new/dir1")), filename);
    mixxx::FileInfo otherFile(QDir(QDir::tempPath() + QStringLiteral("/new")), filename);

    TrackPointer pOldTrack = Track::newTemporary(mixxx::FileAccess(oldFile));
    TrackPointer pNewTrack = Track::newTemporary(mixxx::FileAccess(newFile));
    TrackPointer pOtherTrack = Track::newTemporary(mixxx::FileAccess(otherFile));

    // Arbitrary duration
    pOldTrack->setDuration(135);
    pNewTrack->setDuration(135.7);
    pOtherTrack->setDuration(135.7);

    TrackId oldId = internalCollection()->addTrack(pOldTrack, false);
    TrackId newId = internalCollection()->addTrack(pNewTrack, false);
    internalCollection()->addTrack(pOtherTrack, false);

    // Mark as missing
    QSqlQuery query(dbConnection());
    query.prepare("UPDATE track_locations SET fs_deleted=1 WHERE location=:location");
    query.bindValue(":location", oldFile.location());
    query.exec();

    QList<RelocatedTrack> relocatedTracks;
    QStringList addedTracks(newFile.location());
    bool cancel = false;
    trackDAO.detectMovedTracks(&relocatedTracks, addedTracks, &cancel);

    QSet<TrackId> updatedTrackIds;
    QSet<TrackId> removedTrackIds;
    for (const auto& relocatedTrack : std::as_const(relocatedTracks)) {
        updatedTrackIds.insert(relocatedTrack.updatedTrackRef().getId());
        removedTrackIds.insert(relocatedTrack.deletedTrackId());
    }

    EXPECT_THAT(updatedTrackIds, UnorderedElementsAre(oldId));
    EXPECT_THAT(removedTrackIds, UnorderedElementsAre(newId));

    QSet<QString> trackLocations = trackDAO.getAllTrackLocations();
    EXPECT_THAT(trackLocations, UnorderedElementsAre(newFile.location(), otherFile.location()));
}

// A stick track that is new to this library is baselined by the add path just
// as a known one is by getTrackById(), so the main cue the analyzer gives it
// is not stored on the drive as a DJ override when the track is saved. Before
// issue #24 the add path left it unbaselined, and a track without a baseline
// counts every cue it has as an edit.
TEST_F(TrackDAOTest, AnalyzedNewStickTrackStoresNoCueOverride) {
    if (!fakeUsbIsMounted()) {
        GTEST_SKIP() << "needs a filesystem mounted at " << qPrintable(kFakeUsb);
    }
    const QString trackPath = copyToFakeUsb(QStringLiteral("added-analyzed.wav"));

    TrackPointer pTrack = getOrAddTrackByLocation(trackPath);
    ASSERT_TRUE(pTrack);
    // Without a sample rate the store neither baselines nor saves, and this
    // test would pass for the wrong reason.
    const mixxx::audio::SampleRate sampleRate = pTrack->getSampleRate();
    ASSERT_TRUE(sampleRate.isValid());
    ASSERT_FALSE(pTrack->findCueByType(mixxx::CueType::MainCue));

    // One second of silence, then a square wave at half the sample rate: the
    // analyzer only looks at how loud each sample is.
    const SINT frames = 2 * static_cast<SINT>(sampleRate.value());
    const SINT silentFrames = static_cast<SINT>(sampleRate.value());
    const SINT channels = mixxx::kAnalysisChannels;
    std::vector<CSAMPLE> samples(frames * channels, 0.0f);
    for (SINT i = silentFrames * channels; i < frames * channels; ++i) {
        samples[i] = (i / channels) % 2 == 0 ? 0.5f : -0.5f;
    }
    AnalyzerSilence analyzer(config());
    ASSERT_TRUE(analyzer.initialize(AnalyzerTrack(pTrack), sampleRate, frames));
    ASSERT_TRUE(analyzer.processSamples(samples.data(), static_cast<SINT>(samples.size())));
    analyzer.storeResults(pTrack);
    analyzer.cleanup();
    // The premise: the analyzer really did give the track a main cue.
    ASSERT_TRUE(pTrack->findCueByType(mixxx::CueType::MainCue));

    EXPECT_EQ(TrackCollectionManager::SaveTrackResult::Saved,
            trackCollectionManager()->saveTrack(pTrack));
    // The fixture's collection runs a writer, so a queued write lands later.
    ASSERT_TRUE(FsStoreWriter::flushAll(QDeadlineTimer(FsStoreWriter::kFlushTimeoutMillis)));
    EXPECT_FALSE(QFile::exists(cueStorePath()));

    pTrack.reset();
    QFile::remove(trackPath);
    EXPECT_TRUE(FsCueOverrideStore::clearFilesystemOverrides(kFakeUsb));
}

// Cues a stick carries from another unit are applied to a track the first
// time it is added to this library too, not only when it is loaded back from
// the database. Before issue #24 the add path skipped them.
TEST_F(TrackDAOTest, NewStickTrackGetsTheCueOverrideOnTheDrive) {
    if (!fakeUsbIsMounted()) {
        GTEST_SKIP() << "needs a filesystem mounted at " << qPrintable(kFakeUsb);
    }
    const QString trackPath = copyToFakeUsb(QStringLiteral("added-overridden.wav"));

    // Another unit cued the track: a hot cue in the fourth pad, stored on the
    // stick. A track built outside the library has no baseline, so saving it
    // stores its cue.
    {
        const TrackPointer pElsewhere =
                Track::newTemporary(mixxx::FileAccess(mixxx::FileInfo(trackPath)));
        pElsewhere->setAudioProperties(mixxx::audio::ChannelCount(2),
                mixxx::audio::SampleRate(44100),
                mixxx::audio::Bitrate(),
                mixxx::Duration::fromSeconds(30));
        pElsewhere->createAndAddCue(mixxx::CueType::HotCue,
                mixxx::kHotCueBankStart + 3,
                mixxx::audio::FramePos(12.0 * 44100),
                mixxx::audio::kInvalidFramePos);
        FsCueOverrideStore::flushIfChanged(*pElsewhere);
    }
    ASSERT_TRUE(FsStoreWriter::flushAll(QDeadlineTimer(FsStoreWriter::kFlushTimeoutMillis)));
    ASSERT_TRUE(QFile::exists(cueStorePath()));
    // Read the stick again, as for one that arrives already cued.
    FsCueOverrideStore::forgetFilesystem(kFakeUsb);

    TrackPointer pTrack = getOrAddTrackByLocation(trackPath);
    ASSERT_TRUE(pTrack);
    const mixxx::audio::SampleRate sampleRate = pTrack->getSampleRate();
    ASSERT_TRUE(sampleRate.isValid());

    CuePointer pCue;
    const QList<CuePointer> cues = pTrack->getCuePoints();
    for (const CuePointer& pCandidate : cues) {
        if (pCandidate->getHotCue() == mixxx::kHotCueBankStart + 3) {
            pCue = pCandidate;
        }
    }
    ASSERT_TRUE(pCue);
    EXPECT_DOUBLE_EQ(12.0 * sampleRate.value(), pCue->getPosition().value());

    pTrack.reset();
    QFile::remove(trackPath);
    EXPECT_TRUE(FsCueOverrideStore::clearFilesystemOverrides(kFakeUsb));
}
