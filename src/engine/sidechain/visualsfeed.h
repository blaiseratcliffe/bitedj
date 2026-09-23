#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <atomic>
#include <memory>
#include <vector>

#include "audio/types.h"
#include "control/pollingcontrolproxy.h"
#include "engine/sidechain/sidechainworker.h"
#include "rigtorp/SPSCQueue.h"
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
/// Timing, which is the thing to know before reading a band value. The
/// sidechain does not hand the mix over a buffer at a time: EngineSideChain's
/// thread wakes about every 100 ms and drains its FIFO in one go, so a single
/// process() call carries roughly 100 ms of audio. process() therefore slices
/// what it is given into 33 ms windows and queues one Bands per window; the
/// 30 Hz timer pops one per frame. Every frame consequently carries a
/// distinct window rather than the same batch average repeated, but the band
/// values still trail the beat controls the timer reads live by about 100 ms,
/// plus whatever jitter the drain adds. A sketch that needs a tight beat
/// should key off the deck's beat_active, not off a band rising.
///
/// Threads. process() runs on the sidechain thread and touches nothing but the
/// filters, a scratch buffer, the enabled flag and the producer end of
/// m_bandQueue. The QTimer runs on the main thread, where ControlProxy is
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
    // Half a second of windows. Big enough that a normal ~100 ms batch (three
    // windows) never meets a full queue, small enough that a consumer that
    // has stopped popping cannot hold half a minute of stale audio.
    static constexpr int kQueueCapacity = 16;
    // Trim point and target for drainBands(). Three windows is 100 ms, one
    // sidechain batch, so a backlog is cut back to the newest batch rather
    // than to nothing: popping to empty would stutter every time the drain
    // and the push interleave badly.
    static constexpr size_t kQueueHighWater = 6;
    static constexpr size_t kQueueTrimTo = 3;

    struct Bands {
        float rms[kBandCount] = {0.0f, 0.0f, 0.0f, 0.0f};
        float peak = 0.0f;
    };

    VisualsFeed();
    ~VisualsFeed() override;

    void process(const CSAMPLE* pBuffer, const int iBufferSize) override;
    void shutdown() override;

    bool enabled() const;
    // Main thread only. Drains the window queue by one and returns that
    // window, or the previous one when no new window has arrived. This is
    // not a peek: two calls in a row return two different windows while the
    // queue has them, which is what the three-window drain test relies on.
    Bands latestBands();
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
    // Main thread only. Takes one window into m_lastBands and returns true,
    // or returns false when the queue is empty. Always front() then pop(),
    // never a bare pop(): see the note on the queue's contract in
    // drainBands().
    bool takeWindow();
    // Main thread only. Once the backlog passes kQueueHighWater entries this
    // trims it back to kQueueTrimTo, and it then takes one more window into
    // m_lastBands if one is there. kQueueHighWater is only the trigger; the
    // target is kQueueTrimTo. The trim is what stops a scheduling stall from
    // turning into a lasting delay: without it the timer would spend the next
    // second walking through a backlog it can never catch up on at one window
    // per frame.
    void drainBands();
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

    // The two things the two threads share. m_enabledFlag is written by
    // enabled() on the main thread and read by process() on the sidechain
    // thread. m_bandQueue is a single-producer single-consumer ring: process()
    // pushes one entry per 33 ms window from the sidechain thread and the
    // main thread pops. A latest-value slot was what this used to be, and it
    // threw away two windows out of every three because the sidechain arrives
    // in ~100 ms batches; the queue is what makes each 30 Hz frame a
    // different window.
    mutable std::atomic<bool> m_enabledFlag;
    rigtorp::SPSCQueue<Bands> m_bandQueue;

    // Main-thread state.
    // Mutable: enabled() re-resolves this in place, from a const method,
    // whenever the control has not yet bound to a real CO.
    mutable PollingControlProxy m_enabledControl;
    PollingControlProxy m_numDecksControl;
    QTimer m_timer;
    QElapsedTimer m_clock;
    // The window the last drain handed over, held so a frame built when no
    // new window has arrived repeats the previous one rather than zeroing.
    Bands m_lastBands;
    std::vector<Deck> m_decks;
    std::unique_ptr<ControlProxy> m_pCrossfader;
};
