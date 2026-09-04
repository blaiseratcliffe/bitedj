#include "engine/sync/syncreference.h"

#include "engine/sync/enginesync.h"
#include "engine/sync/syncable.h"
#include "moc_syncreference.cpp"

namespace {
const QString kAppGroup = QStringLiteral("[App]");
} // namespace

SyncReference::SyncReference(EngineSync* pEngineSync, QObject* pParent)
        : QObject(pParent),
          m_pEngineSync(pEngineSync),
          m_pReferenceKey(std::make_unique<ControlObject>(
                  ConfigKey(kAppGroup, QStringLiteral("sync_reference_key")))) {
}

SyncReference::~SyncReference() = default;

void SyncReference::addDeck(const QString& group) {
    auto pDeck = std::make_unique<Deck>();
    pDeck->group = group;
    pDeck->pReference = std::make_unique<ControlObject>(
            ConfigKey(group, QStringLiteral("sync_reference")));

    // AutoConnection on purpose. These controls are written from the engine
    // thread, so the update lands queued on the GUI thread, which is where it
    // has to run: it asks EngineSync for the target and writes controls that
    // skins are bound to. BaseTrackPlayerImpl::slotCloneDeck already calls
    // pickNonSyncSyncTarget from this thread.
    const auto watch = [this, &group](const char* item) {
        auto pProxy = std::make_unique<ControlProxy>(group, item, this);
        pProxy->connectValueChanged(this, [this](double) { update(); });
        return pProxy;
    };

    pDeck->pPlay = watch("play");
    pDeck->pSyncLeader = watch("sync_leader");
    pDeck->pTrackLoaded = watch("track_loaded");
    pDeck->pMainMix = watch("main_mix");
    // Watched so the published key follows a nudge on the reference deck.
    // Recomputing on any deck's key is cheaper than tracking which one is
    // current, and the guarded write below absorbs the rest.
    pDeck->pKey = watch("key");

    m_decks.push_back(std::move(pDeck));

    // A deck added to a running session changes the answer straight away.
    update();
}

void SyncReference::update() {
    // pDontPick is null: unlike the clone and key-sync callers, we are not
    // asking on behalf of a particular deck, so nothing is excluded.
    const Syncable* pTarget = m_pEngineSync->pickNonSyncSyncTarget(nullptr);
    const QString targetGroup = pTarget ? pTarget->getGroup() : QString();

    double referenceKey = 0.0;
    for (const auto& pDeck : m_decks) {
        const bool isReference = !targetGroup.isEmpty() && pDeck->group == targetGroup;
        const double value = isReference ? 1.0 : 0.0;
        // Guarded because every write repolishes any widget bound to it, and
        // these events arrive in bursts when a deck starts or stops.
        if (pDeck->pReference->get() != value) {
            pDeck->pReference->set(value);
        }
        if (isReference) {
            referenceKey = pDeck->pKey->get();
        }
    }

    // Guarded for the same reason, and more sharply: this one repaints a
    // library column, so an unguarded write on every key change would repaint
    // the table continuously while the pitch fader moves.
    if (m_pReferenceKey->get() != referenceKey) {
        m_pReferenceKey->set(referenceKey);
    }
}
