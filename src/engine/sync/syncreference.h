#pragma once

#include <QObject>
#include <QString>
#include <memory>
#include <vector>

#include "control/controlobject.h"
#include "control/controlproxy.h"

class EngineSync;

/// Bite DJ fork: publishes which deck other decks and tracks should be
/// compared against, as `[ChannelN],sync_reference` (1 on that deck, 0 on the
/// rest, and 0 everywhere when nothing is loaded).
///
/// EngineSync already answers this question in pickNonSyncSyncTarget(): the
/// sync leader when it is a deck, else the first playing sync deck, else the
/// first playing deck, else the first stopped one with a valid BPM. That is
/// exactly the deck a DJ is mixing towards, but it lives in the engine with
/// nothing exposed, so no skin can see it. This class does not reimplement the
/// rule, it asks for the answer and republishes it.
///
/// Deliberately separate from EngineSync rather than folded into it: that file
/// is stock upstream Mixxx, and keeping this out of it keeps the fork cheap to
/// rebase.
///
/// `sync_reference` is not `sync_leader`. sync_leader is real engine state and
/// is 0 on every deck until somebody enables sync. sync_reference is always
/// populated once a deck holds a track, because of the fallbacks above. A skin
/// wanting to distinguish "the user chose this" from "we assumed this" reads
/// both: sync_leader > 0 means chosen, sync_reference alone means assumed.
class SyncReference : public QObject {
    Q_OBJECT
  public:
    explicit SyncReference(EngineSync* pEngineSync, QObject* pParent = nullptr);
    ~SyncReference() override;

    /// Register a deck, once, as it is created. Decks are added during startup
    /// and by the Decks preference, never removed.
    void addDeck(const QString& group);

  private:
    /// Recompute every deck's flag. Cheap: one engine query and a compare per
    /// deck, and it only runs on the events below, never per callback.
    void update();

    /// The events that can move the reference. play covers start and stop,
    /// sync_leader covers the leader being taken or reassigned, track_loaded
    /// covers load and eject, and main_mix covers a deck leaving the mix,
    /// which pickNonSyncSyncTarget also tests for.
    ///
    /// Not watched, on purpose: bpm. Its validity can decide the fallback, but
    /// it changes on every pitch move, and a deck that gains a beatgrid after
    /// load is rare next to the churn that watching it would cost.
    struct Deck {
        QString group;
        std::unique_ptr<ControlObject> pReference;
        std::unique_ptr<ControlProxy> pPlay;
        std::unique_ptr<ControlProxy> pSyncLeader;
        std::unique_ptr<ControlProxy> pTrackLoaded;
        std::unique_ptr<ControlProxy> pMainMix;
        /// The deck's *playing* key, so a key nudge on the FX tab or an
        /// unlocked pitch move is followed. Not file_key.
        std::unique_ptr<ControlProxy> pKey;
    };

    EngineSync* const m_pEngineSync;
    std::vector<std::unique_ptr<Deck>> m_decks;

    /// `[App],sync_reference_key`: the reference deck's playing key as a
    /// ChromaticKey numeric value, 0 when there is no reference or its track
    /// has no key.
    ///
    /// Published as one control rather than making readers find the reference
    /// deck and then read its key. The library models need exactly this and
    /// nothing else, so one proxy each is enough and no consumer has to
    /// enumerate decks or know the fallback rule.
    std::unique_ptr<ControlObject> m_pReferenceKey;
};
