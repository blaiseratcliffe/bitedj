#include "library/dao/fsanalysiscache.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QFileInfo>

#include "test/mixxxtest.h"
#include "waveform/waveformfactory.h"

namespace {

const QString kCacheDbName = QStringLiteral("analysis.sqlite");

/// A track on the filesystem that holds the settings dir, which on the
/// appliance means anything under home, including `~/Music`. Its mount root is
/// `/`, which a user process cannot write, so before issue #7 was fixed every
/// load re-analysed it and nothing was stored.
class FsAnalysisCacheTest : public MixxxTest {
  protected:
    void SetUp() override {
        // The fixture's config file, and so the settings path, lives in the
        // test data dir. Putting the track there too keeps both on one
        // filesystem wherever the suite runs.
        m_trackLocation = getTestDataDir().filePath(QStringLiteral("local.mp3"));
        QFile track(m_trackLocation);
        ASSERT_TRUE(track.open(QIODevice::WriteOnly));
        track.close();
    }

    static ConstWaveformPointer pendingWaveform(
            const QString& version, int maxVisualSamples) {
        const auto pWaveform = WaveformPointer(
                new Waveform(44100, 44100, 441, maxVisualSamples));
        pWaveform->setVersion(version);
        pWaveform->setDescription(QStringLiteral("test"));
        pWaveform->setSaveState(Waveform::SaveState::SavePending);
        return pWaveform;
    }

    bool saveBoth(FsAnalysisCache* pCache) {
        return pCache->saveTrackAnalyses(m_trackLocation,
                pendingWaveform(WaveformFactory::currentWaveformVersion(), -1),
                pendingWaveform(WaveformFactory::currentWaveformSummaryVersion(), 1000));
    }

    QString settingsDirDb() const {
        return QDir(config()->getSettingsPath()).filePath(kCacheDbName);
    }

    QString m_trackLocation;
};

// A local track is stored in the settings dir, and a fresh cache (a later load,
// or the next session) finds both waveforms there.
TEST_F(FsAnalysisCacheTest, LocalTrackIsCachedInSettingsDir) {
    {
        FsAnalysisCache cache(config());
        ASSERT_TRUE(saveBoth(&cache));
    }
    EXPECT_TRUE(QFileInfo::exists(settingsDirDb()));

    FsAnalysisCache reloaded(config());
    const QList<AnalysisDao::AnalysisInfo> analyses =
            reloaded.getAnalysesForTrack(m_trackLocation);
    ASSERT_EQ(2, analyses.size());
    bool sawWaveform = false;
    bool sawSummary = false;
    for (const AnalysisDao::AnalysisInfo& analysis : analyses) {
        if (analysis.type == AnalysisDao::TYPE_WAVEFORM) {
            sawWaveform = true;
            EXPECT_QSTRING_EQ(WaveformFactory::currentWaveformVersion(), analysis.version);
        } else if (analysis.type == AnalysisDao::TYPE_WAVESUMMARY) {
            sawSummary = true;
            EXPECT_QSTRING_EQ(
                    WaveformFactory::currentWaveformSummaryVersion(), analysis.version);
        }
        EXPECT_FALSE(analysis.data.isEmpty());
    }
    EXPECT_TRUE(sawWaveform);
    EXPECT_TRUE(sawSummary);
}

// Settings > Clear analysis cache reaches the settings-dir database too, even
// while an instance still holds a connection to it.
TEST_F(FsAnalysisCacheTest, ClearSettingsDirCacheDeletesIt) {
    FsAnalysisCache cache(config());
    ASSERT_TRUE(saveBoth(&cache));
    ASSERT_TRUE(QFileInfo::exists(settingsDirDb()));

    EXPECT_TRUE(FsAnalysisCache::clearSettingsDirCache(config()->getSettingsPath()));
    EXPECT_FALSE(QFileInfo::exists(settingsDirDb()));

    // The open instance reconnects lazily and sees an empty cache, not the
    // deleted file's contents.
    EXPECT_TRUE(cache.getAnalysesForTrack(m_trackLocation).isEmpty());
}

} // namespace
