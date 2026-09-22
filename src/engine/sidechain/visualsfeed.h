#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <memory>
#include <vector>

#include "audio/types.h"
#include "control/controlvalue.h"
#include "control/pollingcontrolproxy.h"
#include "engine/sidechain/sidechainworker.h"
#include "util/types.h"

class ControlProxy;
class EngineFilterBessel4Low;
class EngineFilterBessel4Band;
class EngineFilterBessel4High;

/// Bite DJ: the music signal for the HDMI visuals.
///
/// The FLX6 is the appliance's only audio endpoint and it is playback-only,
/// so nothing outside this process can listen to the mix. This worker sits on
/// the same sidechain the recorder uses, splits the master mix into four bands
/// with the engine's own IIR filters, and at 30 Hz folds the band energies
/// together with what the beatgrid already knows (beat_active, bpm, play, VU,
/// crossfader) into one small JSON frame. VisualsServer pushes those frames to
/// the visuals page over loopback.
///
/// Threads. process() runs on the sidechain thread and touches nothing but the
/// filters, a scratch buffer and one lock-free slot. The QTimer runs on the
/// main thread, where ControlProxy is allowed, and is the only place the frame
/// is built. shutdown() runs on the main thread from ~EngineSideChain, which
/// also deletes this object; the sidechain owns its workers, so nothing else
/// may hold a unique_ptr to one.
class VisualsFeed : public QObject, public SideChainWorker {
    Q_OBJECT
  public:
    static constexpr int kFramesPerSecond = 30;
    static constexpr int kBandCount = 4;

    struct Bands {
        float rms[kBandCount] = {0.0f, 0.0f, 0.0f, 0.0f};
        float peak = 0.0f;
    };

    explicit VisualsFeed(QObject* pParent = nullptr);
    ~VisualsFeed() override;

    void process(const CSAMPLE* pBuffer, const int iBufferSize) override;
    void shutdown() override;

    bool enabled() const;
    Bands latestBands() const;
    QByteArray buildFrame();

  signals:
    void frameReady(const QByteArray& json);

  private slots:
    void onTick();

  private:
    struct Deck {
        std::unique_ptr<ControlProxy> play;
        std::unique_ptr<ControlProxy> bpm;
        std::unique_ptr<ControlProxy> beatActive;
        std::unique_ptr<ControlProxy> beatDistance;
        std::unique_ptr<ControlProxy> vuMeter;
    };

    void retune(mixxx::audio::SampleRate sampleRate);

    // Sidechain-thread state.
    PollingControlProxy m_sampleRateControl;
    PollingControlProxy m_enabledControl;
    mixxx::audio::SampleRate m_sampleRate;
    std::unique_ptr<EngineFilterBessel4Low> m_pLow;
    std::unique_ptr<EngineFilterBessel4Band> m_pLowMid;
    std::unique_ptr<EngineFilterBessel4Band> m_pMid;
    std::unique_ptr<EngineFilterBessel4High> m_pHigh;
    CSAMPLE* m_pScratch;
    double m_sumSquares[kBandCount];
    float m_peak;
    int m_framesAccumulated;

    // The one shared slot.
    ControlValueAtomic<Bands> m_bands;

    // Main-thread state.
    QTimer m_timer;
    QElapsedTimer m_clock;
    std::vector<Deck> m_decks;
    std::unique_ptr<ControlProxy> m_pCrossfader;
};
