#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <atomic>
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
/// filters, a scratch buffer and two lock-free slots, m_enabledFlag and
/// m_bands. The QTimer runs on the main thread, where ControlProxy is
/// allowed, and is the only place the frame is built. shutdown() runs on the
/// main thread from ~EngineSideChain, which also deletes this object; the
/// sidechain owns its workers, so nothing else may hold a unique_ptr to one,
/// and this object must never be given a Qt parent, or the sidechain's delete
/// would race a parent's delete of the same object.
///
/// `[BiteDJ],visuals_enabled` is expected to exist by the time this worker is
/// constructed, but construction order in the appliance is not guaranteed. If
/// the control does not exist yet, enabled() keeps re-resolving it, on the
/// main thread, every time it is called, until the control appears. The
/// sidechain thread never touches that control; process() only reads the
/// atomic flag enabled() publishes.
class VisualsFeed : public QObject, public SideChainWorker {
    Q_OBJECT
  public:
    static constexpr int kFramesPerSecond = 30;
    static constexpr int kBandCount = 4;

    struct Bands {
        float rms[kBandCount] = {0.0f, 0.0f, 0.0f, 0.0f};
        float peak = 0.0f;
    };

    VisualsFeed();
    ~VisualsFeed() override;

    void process(const CSAMPLE* pBuffer, const int iBufferSize) override;
    void shutdown() override;

    bool enabled() const;
    Bands latestBands() const;
    // Main thread only; reads ControlProxy objects.
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
    // Main thread only. Replaces m_decks with `count` fresh deck proxy
    // bundles. Called from the constructor and again from buildFrame()
    // whenever [App],num_decks has moved since the last rebuild.
    void rebuildDecks(int count);

    // Sidechain-thread state.
    PollingControlProxy m_sampleRateControl;
    mixxx::audio::SampleRate m_sampleRate;
    std::unique_ptr<EngineFilterBessel4Low> m_pLow;
    std::unique_ptr<EngineFilterBessel4Band> m_pLowMid;
    std::unique_ptr<EngineFilterBessel4Band> m_pMid;
    std::unique_ptr<EngineFilterBessel4High> m_pHigh;
    CSAMPLE* m_pScratch;
    double m_sumSquares[kBandCount];
    float m_peak;
    int m_framesAccumulated;

    // The two shared, single-writer slots. m_enabledFlag is written by
    // enabled() on the main thread and read by process() on the sidechain
    // thread; m_bands is written by process() on the sidechain thread and
    // read by latestBands() and buildFrame() on the main thread.
    mutable std::atomic<bool> m_enabledFlag;
    ControlValueAtomic<Bands> m_bands;

    // Main-thread state.
    // Mutable: enabled() re-resolves this in place, from a const method,
    // whenever the control has not yet bound to a real CO.
    mutable PollingControlProxy m_enabledControl;
    PollingControlProxy m_numDecksControl;
    QTimer m_timer;
    QElapsedTimer m_clock;
    std::vector<Deck> m_decks;
    std::unique_ptr<ControlProxy> m_pCrossfader;
};
