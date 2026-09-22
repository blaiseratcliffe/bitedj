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

VisualsFeed::VisualsFeed(QObject* pParent)
        : QObject(pParent),
          m_sampleRateControl(kAppGroup, QStringLiteral("samplerate")),
          m_enabledControl(kBiteDjGroup,
                  QStringLiteral("visuals_enabled"),
                  ControlFlag::AllowMissingOrInvalid | ControlFlag::NoWarnIfMissing),
          m_sampleRate(mixxx::audio::SampleRate::fromDouble(m_sampleRateControl.get())),
          m_pScratch(SampleUtil::alloc(EngineSideChain::SIDECHAIN_BUFFER_SIZE)),
          m_sumSquares{0.0, 0.0, 0.0, 0.0},
          m_peak(0.0f),
          m_framesAccumulated(0) {
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

    // Deck proxies only for decks that exist. A proxy on [Channel3] when the
    // engine has two decks logs "getControl returning NULL", which
    // toolchain/skin/check-log.sh reports as a fault.
    PollingControlProxy numDecks(kAppGroup,
            QStringLiteral("num_decks"),
            ControlFlag::AllowMissingOrInvalid | ControlFlag::NoWarnIfMissing);
    const int deckCount = numDecks.valid() ? static_cast<int>(numDecks.get()) : 0;
    for (int i = 1; i <= deckCount; ++i) {
        const QString group = QStringLiteral("[Channel%1]").arg(i);
        Deck deck;
        deck.play = std::make_unique<ControlProxy>(group, QStringLiteral("play"), this);
        deck.bpm = std::make_unique<ControlProxy>(group, QStringLiteral("bpm"), this);
        deck.beatActive = std::make_unique<ControlProxy>(group, QStringLiteral("beat_active"), this);
        deck.beatDistance = std::make_unique<ControlProxy>(group, QStringLiteral("beat_distance"), this);
        deck.vuMeter = std::make_unique<ControlProxy>(group, QStringLiteral("vu_meter"), this);
        m_decks.push_back(std::move(deck));
    }
    m_pCrossfader = std::make_unique<ControlProxy>(kMasterGroup,
            QStringLiteral("crossfader"),
            this,
            ControlFlag::AllowMissingOrInvalid | ControlFlag::NoWarnIfMissing);

    m_clock.start();
    m_timer.setInterval(1000 / kFramesPerSecond);
    connect(&m_timer, &QTimer::timeout, this, &VisualsFeed::onTick);
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
    return m_enabledControl.valid() && m_enabledControl.toBool();
}

VisualsFeed::Bands VisualsFeed::latestBands() const {
    return m_bands.getValue();
}

void VisualsFeed::retune(mixxx::audio::SampleRate sampleRate) {
    m_sampleRate = sampleRate;
    m_pLow->setFrequencyCorners(sampleRate, kBassCorner);
    m_pLowMid->setFrequencyCorners(sampleRate, kBassCorner, kLowMidCorner);
    m_pMid->setFrequencyCorners(sampleRate, kLowMidCorner, kMidCorner);
    m_pHigh->setFrequencyCorners(sampleRate, kMidCorner);
}

void VisualsFeed::process(const CSAMPLE* pBuffer, const int iBufferSize) {
    if (!enabled()) {
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

    EngineFilterIIRBase* filters[kBandCount] = {
            m_pLow.get(), m_pLowMid.get(), m_pMid.get(), m_pHigh.get()};
    for (int b = 0; b < kBandCount; ++b) {
        filters[b]->process(pBuffer, m_pScratch, count);
        double sum = 0.0;
        for (int i = 0; i < count; ++i) {
            sum += static_cast<double>(m_pScratch[i]) * m_pScratch[i];
        }
        m_sumSquares[b] += sum;
    }
    for (int i = 0; i < count; ++i) {
        m_peak = std::max(m_peak, std::abs(pBuffer[i]));
    }
    m_framesAccumulated += count / 2;

    const int framesPerPublish = m_sampleRate.value() / kFramesPerSecond;
    if (m_framesAccumulated >= framesPerPublish) {
        Bands bands;
        const double samples = static_cast<double>(m_framesAccumulated) * 2.0;
        for (int b = 0; b < kBandCount; ++b) {
            bands.rms[b] = static_cast<float>(std::sqrt(m_sumSquares[b] / samples));
            m_sumSquares[b] = 0.0;
        }
        bands.peak = m_peak;
        m_peak = 0.0f;
        m_framesAccumulated = 0;
        m_bands.setValue(bands);
    }
}

QByteArray VisualsFeed::buildFrame() {
    const Bands bands = m_bands.getValue();
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
