// The visuals feed driven the way the sidechain thread drives it: interleaved
// stereo buffers in, a JSON frame out. What it is here to catch is a band
// filter wired to the wrong corner, a frame whose shape drifts from what
// res/visuals/feed.js parses, a deck proxy created for a deck that does not
// exist (which logs a fault the appliance's log check fails on), a deck count
// that goes stale after [App],num_decks changes, a visuals_enabled control
// that shows up after this worker is already constructed, and the window
// queue collapsing back into a single latest-value slot, which is what made
// the feed publish the same 100 ms average three frames running.
//
// latestBands() is a drain, not a peek: each call hands over the next queued
// 33 ms window and only repeats itself once the queue is empty. Tests that
// feed a long steady tone and then read once are unaffected, because every
// window of a steady tone is the same; the two drain tests at the bottom are
// the ones that depend on the ordering.
#include "engine/sidechain/visualsfeed.h"

#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QTest>
#include <cmath>
#include <memory>
#include <vector>

#include "control/controlobject.h"
#include "engine/engine.h"
#include "engine/sidechain/visualsserver.h"
#include "test/mixxxtest.h"
#include "util/types.h"

namespace {

constexpr int kSampleRate = 44100;
constexpr int kFramesPerBuffer = 1024;
constexpr int kDecks = 2;
// The fixture backs this many decks with real controls, so that a test can
// raise [App],num_decks up to this count without VisualsFeed logging a fault
// for a channel that does not exist.
constexpr int kMaxTestDecks = 4;

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
        for (int i = 1; i <= kMaxTestDecks; ++i) {
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
// The analysis must still be correct, not just crash-free: feed the same
// bass sine as BassSineLandsInBandZero, but in buffers whose length is odd.
TEST_F(VisualsFeedTest, OddSampleCountIsSafe) {
    constexpr int kOddBufferSize = 2047;
    std::vector<CSAMPLE> buffer(kOddBufferSize);
    double phase = 0.0;
    const double step = 2.0 * M_PI * 60.0 / kSampleRate;
    for (int b = 0; b < 100; ++b) {
        for (int i = 0; i + 1 < kOddBufferSize; i += 2) {
            const CSAMPLE v = static_cast<CSAMPLE>(0.5 * std::sin(phase));
            buffer[i] = v;
            buffer[i + 1] = v;
            phase += step;
        }
        m_pFeed->process(buffer.data(), static_cast<int>(buffer.size()));
    }
    EXPECT_EQ(0, dominantBand());
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

// [App],num_decks can change after construction (a controller reconfiguration,
// or the FLX6 profile switching deck count). buildFrame() must notice and
// rebuild the deck proxy list rather than serving a stale count forever.
TEST_F(VisualsFeedTest, DeckCountFollowsNumDecks) {
    QJsonObject frame = QJsonDocument::fromJson(m_pFeed->buildFrame()).object();
    EXPECT_EQ(kDecks, frame.value("decks").toArray().size());

    m_pNumDecks->set(4);
    frame = QJsonDocument::fromJson(m_pFeed->buildFrame()).object();
    EXPECT_EQ(4, frame.value("decks").toArray().size());

    m_pNumDecks->set(0);
    frame = QJsonDocument::fromJson(m_pFeed->buildFrame()).object();
    EXPECT_EQ(0, frame.value("decks").toArray().size());
}

// If [BiteDJ],visuals_enabled does not exist yet when VisualsFeed is
// constructed, it must not stay dead forever: the appliance's own startup
// order is not guaranteed to create it first.
TEST_F(VisualsFeedTest, EnabledControlIsPickedUpAfterConstruction) {
    // The shared fixture already built a feed with the control present.
    // Tear both down and rebuild in the order this test actually needs: the
    // feed constructed first, the control created only afterward.
    m_pFeed.reset();
    m_pEnabled.reset();

    m_pFeed = std::make_unique<VisualsFeed>();
    QSignalSpy spy(m_pFeed.get(), &VisualsFeed::frameReady);

    QTest::qWait(100);
    EXPECT_EQ(0, spy.count());

    m_pEnabled = std::make_unique<ControlObject>(
            ConfigKey(QStringLiteral("[BiteDJ]"), QStringLiteral("visuals_enabled")));
    m_pEnabled->set(1.0);

    for (int i = 0; i < 10; ++i) {
        QTest::qWait(20);
    }
    EXPECT_GT(spy.count(), 0);
}

TEST_F(VisualsFeedTest, ShutdownStopsTheTimer) {
    QSignalSpy spy(m_pFeed.get(), &VisualsFeed::frameReady);
    m_pFeed->shutdown();
    m_pEnabled->set(1.0);
    QTest::qWait(150);
    EXPECT_EQ(0, spy.count());
}

TEST_F(VisualsFeedTest, FrameTimeIsMonotonic) {
    const QJsonObject first = QJsonDocument::fromJson(m_pFeed->buildFrame()).object();
    QTest::qWait(20);
    const QJsonObject second = QJsonDocument::fromJson(m_pFeed->buildFrame()).object();
    EXPECT_GE(second.value("t").toDouble(), first.value("t").toDouble());
}

// One sidechain wake-up carries about 100 ms of audio, not one audio buffer:
// EngineSideChain::run() drains its FIFO every 100 ms, so process() sees
// roughly 4410 frames at 44.1 kHz. Three 33 ms windows must come back out of
// it, in order, rather than one average of the lot. The three thirds are the
// same tone at three amplitudes, so reading three rising peaks back is proof
// that three separate windows were analysed and queued, and that they were
// consumed oldest first.
TEST_F(VisualsFeedTest, OneDrainBatchYieldsThreeDistinctWindows) {
    constexpr int kFramesPerWindow = kSampleRate / VisualsFeed::kFramesPerSecond;
    constexpr int kFrames = kFramesPerWindow * 3;
    const double amplitudes[3] = {0.2, 0.5, 0.8};

    std::vector<CSAMPLE> buffer(kFrames * mixxx::kEngineChannelCount);
    double phase = 0.0;
    const double step = 2.0 * M_PI * 60.0 / kSampleRate;
    for (int f = 0; f < kFrames; ++f) {
        const double amplitude = amplitudes[f / kFramesPerWindow];
        const CSAMPLE v = static_cast<CSAMPLE>(amplitude * std::sin(phase));
        buffer[2 * f] = v;
        buffer[2 * f + 1] = v;
        phase += step;
    }
    m_pFeed->process(buffer.data(), static_cast<int>(buffer.size()));

    const float first = m_pFeed->latestBands().peak;
    const float second = m_pFeed->latestBands().peak;
    const float third = m_pFeed->latestBands().peak;
    EXPECT_NEAR(0.2f, first, 0.06f);
    EXPECT_NEAR(0.5f, second, 0.06f);
    EXPECT_NEAR(0.8f, third, 0.06f);
    EXPECT_LT(first, second);
    EXPECT_LT(second, third);
}

// The other half of the same bargain: a stall on the main thread must not
// turn into a lasting delay. Twenty windows arrive in one call, far more than
// the 30 Hz timer could catch up on at one window per frame, so two separate
// bounds apply and this pins both.
//
// The producer bound comes first: the queue holds kQueueCapacity entries, so
// try_push accepts windows 1 to 16 and drops 17 to 20. Window 16 is the
// newest thing in the queue, and nothing read here can ever be newer.
//
// The consumer bound follows: the first read sees 16 queued, which is over
// kQueueHighWater, trims back to kQueueTrimTo and then takes one. So the
// readable windows are 14, 15 and 16, at amplitudes 0.56, 0.60 and 0.64, and
// read four onwards must find the queue empty and repeat window 16 exactly.
//
// Pinning the values rather than only their order is deliberate. The first
// version of the drain popped without going through front(), which left the
// queue's consumer-side cache stale; reads one to three still looked right,
// and read four returned a slot that had never been written, as a denormal
// float. Only an exact-value assertion catches that shape of failure.
TEST_F(VisualsFeedTest, BacklogIsBoundedToThreeWindows) {
    constexpr int kFramesPerWindow = kSampleRate / VisualsFeed::kFramesPerSecond;
    constexpr int kWindows = 20;
    constexpr int kFrames = kFramesPerWindow * kWindows;
    constexpr double kAmplitudeStep = 0.04;

    std::vector<CSAMPLE> buffer(kFrames * mixxx::kEngineChannelCount);
    double phase = 0.0;
    const double step = 2.0 * M_PI * 60.0 / kSampleRate;
    for (int f = 0; f < kFrames; ++f) {
        // A different amplitude per window, so a repeated or stale window is
        // visible rather than hiding behind an identical value.
        const double amplitude = kAmplitudeStep * (f / kFramesPerWindow + 1);
        const CSAMPLE v = static_cast<CSAMPLE>(amplitude * std::sin(phase));
        buffer[2 * f] = v;
        buffer[2 * f + 1] = v;
        phase += step;
    }
    ASSERT_GT(kWindows, VisualsFeed::kQueueCapacity)
            << "the test only bounds the producer if it overfills the queue";
    m_pFeed->process(buffer.data(), static_cast<int>(buffer.size()));

    std::vector<float> peaks;
    for (int i = 0; i < 6; ++i) {
        peaks.push_back(m_pFeed->latestBands().peak);
    }

    // Window numbers are 1-based here, matching the amplitude ramp above.
    const int newestQueued = VisualsFeed::kQueueCapacity;
    const int readable = static_cast<int>(VisualsFeed::kQueueTrimTo);
    ASSERT_EQ(3, readable) << "this test spells out three reads";
    for (int i = 0; i < readable; ++i) {
        const float expected =
                static_cast<float>(kAmplitudeStep * (newestQueued - readable + 1 + i));
        EXPECT_NEAR(expected, peaks[i], 0.02f) << "read " << i + 1;
    }
    EXPECT_LT(peaks[0], peaks[1]);
    EXPECT_LT(peaks[1], peaks[2]);
    // Nothing is left, so every later read repeats the newest queued window.
    // Exact equality, because a repeat is a copy of the same struct.
    EXPECT_FLOAT_EQ(peaks[2], peaks[3]);
    EXPECT_FLOAT_EQ(peaks[2], peaks[4]);
    EXPECT_FLOAT_EQ(peaks[2], peaks[5]);
}

class VisualsServerTest : public VisualsFeedTest {
  protected:
    void SetUp() override {
        VisualsFeedTest::SetUp();
        m_pServer = std::make_unique<VisualsServer>(m_pFeed.get(), 0);
        ASSERT_GT(m_pServer->serverPort(), 0);
    }

    // Connects, sends one request, and returns everything received within
    // `waitMs`. The event loop is pumped by hand: the test has no running loop.
    QByteArray request(const QByteArray& line, int waitMs = 200) {
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, m_pServer->serverPort());
        if (!socket.waitForConnected(1000)) {
            return QByteArray("CONNECT FAILED");
        }
        socket.write(line);
        socket.flush();
        QByteArray received;
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < waitMs) {
            application()->processEvents();
            if (socket.waitForReadyRead(10)) {
                received += socket.readAll();
            }
            if (m_pendingFrame.size() > 0 && m_pServer->clientCount() > 0) {
                m_pServer->broadcastFrame(m_pendingFrame);
                m_pendingFrame.clear();
            }
        }
        return received;
    }

    std::unique_ptr<VisualsServer> m_pServer;
    QByteArray m_pendingFrame;
};

TEST_F(VisualsServerTest, EventsStreamDeliversFrame) {
    m_pendingFrame = "{\"t\":1}";
    const QByteArray got = request("GET /events HTTP/1.1\r\nHost: x\r\n\r\n");
    EXPECT_TRUE(got.startsWith("HTTP/1.1 200")) << got.constData();
    EXPECT_TRUE(got.contains("Content-Type: text/event-stream")) << got.constData();
    EXPECT_TRUE(got.contains("Access-Control-Allow-Origin: *")) << got.constData();
    EXPECT_EQ(1, got.count("data: {\"t\":1}\n\n")) << got.constData();
    // The fixture's feed is enabled with its 33 ms timer running, so real
    // frames land during the wait alongside the injected one. The feed's
    // JSON is compact with alphabetical keys, so a real frame begins
    // `{"bands"`: this is the only check here that the frameReady ->
    // broadcastFrame connection itself works, not just direct injection.
    EXPECT_GE(got.count("data: {\"bands\""), 1) << got.constData();
}

TEST_F(VisualsServerTest, StatusReportsEnabledAndClients) {
    const QByteArray got = request("GET /status HTTP/1.1\r\n\r\n");
    EXPECT_TRUE(got.startsWith("HTTP/1.1 200")) << got.constData();
    EXPECT_TRUE(got.contains("Content-Type: application/json")) << got.constData();
    EXPECT_TRUE(got.contains("{\"enabled\":1,\"clients\":0}")) << got.constData();
}

TEST_F(VisualsServerTest, StatusReflectsDisabled) {
    m_pEnabled->set(0.0);
    const QByteArray got = request("GET /status HTTP/1.1\r\n\r\n");
    EXPECT_TRUE(got.contains("{\"enabled\":0,\"clients\":0}")) << got.constData();
}

TEST_F(VisualsServerTest, UnknownPathIs404) {
    const QByteArray got = request("GET /nope HTTP/1.1\r\n\r\n");
    EXPECT_TRUE(got.startsWith("HTTP/1.1 404")) << got.constData();
}

TEST_F(VisualsServerTest, OversizedRequestIsRejected) {
    const QByteArray got = request(QByteArray(5000, 'A'));
    EXPECT_TRUE(got.startsWith("HTTP/1.1 431")) << got.constData();
}

} // namespace
