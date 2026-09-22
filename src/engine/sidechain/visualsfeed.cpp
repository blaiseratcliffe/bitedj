#include "engine/sidechain/visualsfeed.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cmath>

#include "control/controlproxy.h"
#include "engine/engine.h"
#include "engine/filters/enginefilterbessel4.h"
#include "engine/sidechain/enginesidechain.h"
#include "moc_visualsfeed.cpp"
#include "util/sample.h"

namespace {

constexpr double kBassCorner = 120.0;
constexpr double kLowMidCorner = 500.0;
constexpr double kMidCorner = 2500.0;

const QString kAppGroup = QStringLiteral("[App]");
const QString kMasterGroup = QStringLiteral("[Master]");
const QString kBiteDjGroup = QStringLiteral("[BiteDJ]");

} // namespace

VisualsFeed::VisualsFeed()
        : QObject(nullptr),
          m_sampleRateControl(kAppGroup,
                  QStringLiteral("samplerate"),
                  ControlFlag::AllowMissingOrInvalid | ControlFlag::NoWarnIfMissing),
          m_sampleRate(mixxx::audio::SampleRate::fromDouble(m_sampleRateControl.get())),
          m_pScratch(SampleUtil::alloc(EngineSideChain::SIDECHAIN_BUFFER_SIZE)),
          m_sumSquares{0.0, 0.0, 0.0, 0.0},
          m_peak(0.0f),
          m_framesAccumulated(0),
          m_enabledFlag(false),
          m_bandQueue(kQueueCapacity),
          m_enabledControl(kBiteDjGroup,
                  QStringLiteral("visuals_enabled"),
                  ControlFlag::AllowMissingOrInvalid | ControlFlag::NoWarnIfMissing),
          m_numDecksControl(kAppGroup,
                  QStringLiteral("num_decks"),
                  ControlFlag::AllowMissingOrInvalid | ControlFlag::NoWarnIfMissing) {
    if (!m_sampleRate.isValid()) {
        m_sampleRate = mixxx::audio::SampleRate(44100);
    }
    m_pLow = std::make_unique<EngineFilterBessel4Low>(m_sampleRate, kBassCorner);
    m_pLowMid = std::make_unique<EngineFilterBessel4Band>(m_sampleRate, kBassCorner, kLowMidCorner);
    m_pMid = std::make_unique<EngineFilterBessel4Band>(m_sampleRate, kLowMidCorner, kMidCorner);
    m_pHigh = std::make_unique<EngineFilterBessel4High>(m_sampleRate, kMidCorner);
    // A fresh filter emits half a buffer of silence while it ramps in. Nothing
    // here needs that click protection and the tests do not want it.
    m_pLow->assumeSettled();
    m_pLowMid->assumeSettled();
    m_pMid->assumeSettled();
    m_pHigh->assumeSettled();

    rebuildDecks(m_numDecksControl.valid() ? static_cast<int>(m_numDecksControl.get()) : 0);

    m_pCrossfader = std::make_unique<ControlProxy>(kMasterGroup,
            QStringLiteral("crossfader"),
            this,
            ControlFlag::AllowMissingOrInvalid | ControlFlag::NoWarnIfMissing);

    m_clock.start();
    m_timer.setInterval(1000 / kFramesPerSecond);
    connect(&m_timer, &QTimer::timeout, this, &VisualsFeed::onTick);
    // Seed m_enabledFlag before the sidechain thread's first process() call;
    // this also makes the first re-resolve attempt if the CO is not up yet.
    enabled();
    m_timer.start();
}

VisualsFeed::~VisualsFeed() {
    m_timer.stop();
    SampleUtil::free(m_pScratch);
}

void VisualsFeed::shutdown() {
    m_timer.stop();
}

bool VisualsFeed::enabled() const {
    if (!m_enabledControl.valid()) {
        // Not bound to a real CO yet, either because construction ran before
        // [BiteDJ],visuals_enabled existed, or it still does not exist.
        // Try again; a PollingControlProxy never repoints itself once made.
        m_enabledControl = PollingControlProxy(kBiteDjGroup,
                QStringLiteral("visuals_enabled"),
                ControlFlag::AllowMissingOrInvalid | ControlFlag::NoWarnIfMissing);
    }
    const bool value = m_enabledControl.valid() && m_enabledControl.toBool();
    m_enabledFlag.store(value, std::memory_order_relaxed);
    return value;
}

void VisualsFeed::drainBands() {
    // Only trim once the backlog is clearly a stall, not ordinary batch
    // jitter: a fresh ~100 ms batch leaves three windows queued and must be
    // played out in full.
    if (m_bandQueue.size() > kQueueHighWater) {
        while (m_bandQueue.size() > kQueueTrimTo) {
            m_bandQueue.pop();
        }
    }
    if (Bands* pFront = m_bandQueue.front()) {
        m_lastBands = *pFront;
        m_bandQueue.pop();
    }
}

VisualsFeed::Bands VisualsFeed::latestBands() {
    drainBands();
    return m_lastBands;
}

void VisualsFeed::retune(mixxx::audio::SampleRate sampleRate) {
    m_sampleRate = sampleRate;
    m_pLow->setFrequencyCorners(sampleRate, kBassCorner);
    m_pLowMid->setFrequencyCorners(sampleRate, kBassCorner, kLowMidCorner);
    m_pMid->setFrequencyCorners(sampleRate, kLowMidCorner, kMidCorner);
    m_pHigh->setFrequencyCorners(sampleRate, kMidCorner);
}

void VisualsFeed::rebuildDecks(int count) {
    count = std::max(0, count);
    m_decks.clear();
    for (int i = 1; i <= count; ++i) {
        const QString group = QStringLiteral("[Channel%1]").arg(i);
        Deck deck;
        deck.play = std::make_unique<ControlProxy>(group, QStringLiteral("play"), this);
        deck.bpm = std::make_unique<ControlProxy>(group, QStringLiteral("bpm"), this);
        deck.beatActive = std::make_unique<ControlProxy>(group, QStringLiteral("beat_active"), this);
        deck.beatDistance = std::make_unique<ControlProxy>(group, QStringLiteral("beat_distance"), this);
        deck.vuMeter = std::make_unique<ControlProxy>(group, QStringLiteral("vu_meter"), this);
        m_decks.push_back(std::move(deck));
    }
}

void VisualsFeed::process(const CSAMPLE* pBuffer, const int iBufferSize) {
    if (!m_enabledFlag.load(std::memory_order_relaxed)) {
        for (int b = 0; b < kBandCount; ++b) {
            m_sumSquares[b] = 0.0;
        }
        m_peak = 0.0f;
        m_framesAccumulated = 0;
        return;
    }
    // Interleaved stereo, and the FIFO can hand over an odd count after a
    // partial write; the filters step by two.
    const int count = std::min(iBufferSize, EngineSideChain::SIDECHAIN_BUFFER_SIZE) & ~1;
    if (count <= 0) {
        return;
    }

    const auto sampleRate = mixxx::audio::SampleRate::fromDouble(m_sampleRateControl.get());
    if (sampleRate.isValid() && sampleRate != m_sampleRate) {
        retune(sampleRate);
    }

    // The sidechain thread drains its FIFO about every 100 ms, so `count` is
    // typically four thousand-odd frames rather than one audio buffer. Folding
    // all of that into one published value would give the 30 Hz timer the same
    // 100 ms average three frames running, which is what this loop exists to
    // avoid: walk the buffer in 33 ms windows and queue one Bands per window,
    // carrying whatever does not fill a window over to the next call in the
    // same accumulators. The filters are stateful and are stepped over each
    // slice in order, so slicing changes nothing about their output.
    const int framesPerPublish =
            std::max(1, static_cast<int>(m_sampleRate.value()) / kFramesPerSecond);
    const int samplesPerPublish = framesPerPublish * mixxx::kEngineChannelCount;

    EngineFilterIIRBase* filters[kBandCount] = {
            m_pLow.get(), m_pLowMid.get(), m_pMid.get(), m_pHigh.get()};

    int offset = 0;
    while (offset < count) {
        // Every term here is even: count is masked even, samplesPerPublish is
        // a frame count doubled, and m_framesAccumulated counts whole frames.
        // So the filters never see a half frame.
        const int accumulated = m_framesAccumulated * mixxx::kEngineChannelCount;
        const int chunk = std::min(count - offset, samplesPerPublish - accumulated);
        for (int b = 0; b < kBandCount; ++b) {
            filters[b]->process(pBuffer + offset, m_pScratch, chunk);
            double sum = 0.0;
            for (int i = 0; i < chunk; ++i) {
                sum += static_cast<double>(m_pScratch[i]) * m_pScratch[i];
            }
            m_sumSquares[b] += sum;
        }
        for (int i = offset; i < offset + chunk; ++i) {
            m_peak = std::max(m_peak, std::abs(pBuffer[i]));
        }
        m_framesAccumulated += chunk / mixxx::kEngineChannelCount;
        offset += chunk;

        if (m_framesAccumulated >= framesPerPublish) {
            Bands bands;
            const double samples = static_cast<double>(m_framesAccumulated) *
                    static_cast<double>(mixxx::kEngineChannelCount);
            for (int b = 0; b < kBandCount; ++b) {
                bands.rms[b] = static_cast<float>(std::sqrt(m_sumSquares[b] / samples));
                m_sumSquares[b] = 0.0;
            }
            bands.peak = m_peak;
            m_peak = 0.0f;
            m_framesAccumulated = 0;
            // try_push, not push: if the main thread has stopped popping the
            // sidechain drops the window and carries on. Blocking here would
            // stall the thread that also feeds the recorder. A dropped window
            // is a dropped animation frame and nothing more, so the result is
            // deliberately ignored.
            static_cast<void>(m_bandQueue.try_push(bands));
        }
    }
}

QByteArray VisualsFeed::buildFrame() {
    const int deckCount = m_numDecksControl.valid() ? static_cast<int>(m_numDecksControl.get()) : 0;
    if (deckCount != static_cast<int>(m_decks.size())) {
        rebuildDecks(deckCount);
    }

    // One window per frame, so the page sees 30 distinct 33 ms analyses a
    // second instead of the same batch average three times running.
    drainBands();
    const Bands bands = m_lastBands;
    QJsonObject frame;
    frame.insert(QStringLiteral("t"), static_cast<double>(m_clock.elapsed()));
    QJsonArray bandArray;
    for (int b = 0; b < kBandCount; ++b) {
        bandArray.append(static_cast<double>(bands.rms[b]));
    }
    frame.insert(QStringLiteral("bands"), bandArray);
    frame.insert(QStringLiteral("peak"), static_cast<double>(bands.peak));
    frame.insert(QStringLiteral("xf"), m_pCrossfader->valid() ? m_pCrossfader->get() : 0.0);
    QJsonArray decks;
    for (const Deck& deck : m_decks) {
        QJsonObject d;
        d.insert(QStringLiteral("play"), deck.play->toBool() ? 1 : 0);
        d.insert(QStringLiteral("bpm"), deck.bpm->get());
        d.insert(QStringLiteral("beat"), static_cast<int>(deck.beatActive->get()));
        d.insert(QStringLiteral("bd"), deck.beatDistance->get());
        d.insert(QStringLiteral("vu"), deck.vuMeter->get());
        decks.append(d);
    }
    frame.insert(QStringLiteral("decks"), decks);
    return QJsonDocument(frame).toJson(QJsonDocument::Compact);
}

void VisualsFeed::onTick() {
    if (!enabled()) {
        return;
    }
    emit frameReady(buildFrame());
}
