// The visuals feed driven the way the sidechain thread drives it: interleaved
// stereo buffers in, a JSON frame out. What it is here to catch is a band
// filter wired to the wrong corner, a frame whose shape drifts from what
// res/visuals/feed.js parses, and a deck proxy created for a deck that does
// not exist (which logs a fault the appliance's log check fails on).
#include "engine/sidechain/visualsfeed.h"

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <cmath>
#include <memory>
#include <vector>

#include "control/controlobject.h"
#include "engine/engine.h"
#include "test/mixxxtest.h"
#include "util/types.h"

namespace {

constexpr int kSampleRate = 44100;
constexpr int kFramesPerBuffer = 1024;
constexpr int kDecks = 2;

class VisualsFeedTest : public MixxxTest {
  protected:
    void SetUp() override {
        m_pSampleRate = std::make_unique<ControlObject>(
                ConfigKey(QStringLiteral("[App]"), QStringLiteral("samplerate")));
        m_pSampleRate->set(kSampleRate);
        m_pNumDecks = std::make_unique<ControlObject>(
                ConfigKey(QStringLiteral("[App]"), QStringLiteral("num_decks")));
        m_pNumDecks->set(kDecks);
        m_pEnabled = std::make_unique<ControlObject>(
                ConfigKey(QStringLiteral("[BiteDJ]"), QStringLiteral("visuals_enabled")));
        m_pEnabled->set(1.0);
        m_pCrossfader = std::make_unique<ControlObject>(
                ConfigKey(QStringLiteral("[Master]"), QStringLiteral("crossfader")));
        for (int i = 1; i <= kDecks; ++i) {
            const QString group = QStringLiteral("[Channel%1]").arg(i);
            for (const char* key : {"play", "bpm", "beat_active", "beat_distance", "vu_meter"}) {
                m_deckControls.push_back(std::make_unique<ControlObject>(
                        ConfigKey(group, QString::fromLatin1(key))));
            }
        }
        m_pFeed = std::make_unique<VisualsFeed>();
    }

    void feedSine(double hz, int buffers, double amplitude = 0.5) {
        std::vector<CSAMPLE> buffer(kFramesPerBuffer * mixxx::kEngineChannelCount);
        double phase = 0.0;
        const double step = 2.0 * M_PI * hz / kSampleRate;
        for (int b = 0; b < buffers; ++b) {
            for (int f = 0; f < kFramesPerBuffer; ++f) {
                const CSAMPLE v = static_cast<CSAMPLE>(amplitude * std::sin(phase));
                buffer[2 * f] = v;
                buffer[2 * f + 1] = v;
                phase += step;
            }
            m_pFeed->process(buffer.data(), static_cast<int>(buffer.size()));
        }
    }

    void feedSilence(int buffers) {
        std::vector<CSAMPLE> buffer(kFramesPerBuffer * mixxx::kEngineChannelCount, 0.0f);
        for (int b = 0; b < buffers; ++b) {
            m_pFeed->process(buffer.data(), static_cast<int>(buffer.size()));
        }
    }

    int dominantBand() {
        const VisualsFeed::Bands bands = m_pFeed->latestBands();
        int best = 0;
        for (int i = 1; i < VisualsFeed::kBandCount; ++i) {
            if (bands.rms[i] > bands.rms[best]) {
                best = i;
            }
        }
        return best;
    }

    std::unique_ptr<ControlObject> m_pSampleRate;
    std::unique_ptr<ControlObject> m_pNumDecks;
    std::unique_ptr<ControlObject> m_pEnabled;
    std::unique_ptr<ControlObject> m_pCrossfader;
    std::vector<std::unique_ptr<ControlObject>> m_deckControls;
    std::unique_ptr<VisualsFeed> m_pFeed;
};

// 100 buffers is 2.3 s of audio: many 30 Hz publish windows, so the slot holds
// a settled analysis rather than the filter's start-up transient.
TEST_F(VisualsFeedTest, BassSineLandsInBandZero) {
    feedSine(60.0, 100);
    EXPECT_EQ(0, dominantBand());
    EXPECT_GT(m_pFeed->latestBands().rms[0], 0.1f);
}

TEST_F(VisualsFeedTest, HighSineLandsInBandThree) {
    feedSine(5000.0, 100);
    EXPECT_EQ(3, dominantBand());
}

TEST_F(VisualsFeedTest, MidSineLandsInBandTwo) {
    feedSine(1000.0, 100);
    EXPECT_EQ(2, dominantBand());
}

TEST_F(VisualsFeedTest, SilenceIsZero) {
    feedSilence(100);
    const VisualsFeed::Bands bands = m_pFeed->latestBands();
    for (int i = 0; i < VisualsFeed::kBandCount; ++i) {
        EXPECT_FLOAT_EQ(0.0f, bands.rms[i]);
    }
    EXPECT_FLOAT_EQ(0.0f, bands.peak);
}

TEST_F(VisualsFeedTest, PeakTracksAmplitude) {
    feedSine(60.0, 100, 0.8);
    EXPECT_NEAR(0.8f, m_pFeed->latestBands().peak, 0.05f);
}

// The FIFO can hand the sidechain an odd sample count after a partial write.
TEST_F(VisualsFeedTest, OddSampleCountIsSafe) {
    std::vector<CSAMPLE> buffer(2047, 0.25f);
    m_pFeed->process(buffer.data(), static_cast<int>(buffer.size()));
    SUCCEED();
}

TEST_F(VisualsFeedTest, FrameHasDocumentedShape) {
    feedSine(60.0, 100);
    ControlObject::set(ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("play")), 1.0);
    ControlObject::set(ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("bpm")), 174.0);
    ControlObject::set(ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("beat_active")), 1.0);
    ControlObject::set(ConfigKey(QStringLiteral("[Master]"), QStringLiteral("crossfader")), -0.5);

    const QByteArray json = m_pFeed->buildFrame();
    const QJsonObject frame = QJsonDocument::fromJson(json).object();
    ASSERT_FALSE(frame.isEmpty()) << json.constData();
    EXPECT_TRUE(frame.contains("t"));
    EXPECT_EQ(4, frame.value("bands").toArray().size());
    EXPECT_TRUE(frame.contains("peak"));
    EXPECT_DOUBLE_EQ(-0.5, frame.value("xf").toDouble());

    const QJsonArray decks = frame.value("decks").toArray();
    ASSERT_EQ(kDecks, decks.size());
    const QJsonObject deck1 = decks.at(0).toObject();
    EXPECT_EQ(1, deck1.value("play").toInt());
    EXPECT_DOUBLE_EQ(174.0, deck1.value("bpm").toDouble());
    EXPECT_EQ(1, deck1.value("beat").toInt());
    EXPECT_TRUE(deck1.contains("bd"));
    EXPECT_TRUE(deck1.contains("vu"));
}

TEST_F(VisualsFeedTest, DisabledProducesNoFrames) {
    QSignalSpy spy(m_pFeed.get(), &VisualsFeed::frameReady);
    m_pEnabled->set(0.0);
    // Pump long enough for several timer ticks.
    for (int i = 0; i < 10; ++i) {
        QTest::qWait(20);
    }
    EXPECT_EQ(0, spy.count());
    m_pEnabled->set(1.0);
    for (int i = 0; i < 10; ++i) {
        QTest::qWait(20);
    }
    EXPECT_GT(spy.count(), 0);
}

} // namespace
