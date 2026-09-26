#pragma once

#include <QAtomicPointer>
#include <QByteArray>
#include <QFileSystemWatcher>
#include <QHash>
#include <QList>
#include <QMap>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QTimer>
#include <memory>

#include "preferences/usersettings.h"

class ControlObject;
class ControlProxy;

// Bite DJ: the panel's side of the visuals library (fork issue #13). The
// admin service on port 7380 (pi/bin/bitedj-visuals-admin, appliance repo)
// is the only writer of ~/.bitedj-visuals-sets.json; this class reads it,
// lists the set names for the Visuals page, and owns which set is active.
//
// Three controls live here. [BiteDJ],visuals_set is the active set's id,
// persisted in mixxx.cfg, 0 meaning the built-in "Everything" set that is
// never stored in the file. [BiteDJ],visuals_set_rev starts at the app
// start's epoch seconds and goes up by one per change to the file;
// VisualsFeed carries both in every frame as "set" and "setRev", and
// res/visuals/sets.js rereads the file when either moves. The epoch start
// matters because Chromium stays up while the app restarts
// (pi/bin/bitedj-visuals): a rev that began at 0 again would repeat a pair
// the running page already saw, and an edit made during the restart would
// never be read. [BiteDJ],visuals_page is the Visuals page's sub-page
// (kPage* below) for a WidgetStack in the skin; it is not persisted, and it
// goes back to 0 whenever [SettingsTab],current changes, because a
// WWidgetStack reopens on whatever page its control holds.
//
// Picking a set writes its knobs into the Visuals page's own controls, which
// SystemSettings owns and persists; the panel's later changes are never
// written back. The knobs are applied when visuals_set changes, when the
// active set is picked again (a no-op write fires nothing, control.cpp:294,
// so selectRow() applies them itself), and when the active set's stored
// knobs change in the file. Never at construction: mixxx.cfg already holds
// whatever the panel last set, and applying the set's knobs at every boot
// would undo it. Knob writes go through the static ControlObject::set(),
// which passes a null sender; a member set() on the object getControl()
// returns would pass that object as the sender,
// ControlObject::privateValueChanged would swallow valueChanged, and
// SystemSettings' persistence would never hear of the change.
//
// Mixxx never reads sequence entries: the page does all ordering. A file
// that cannot be parsed lists only Everything and leaves visuals_set alone,
// so fixing the file restores the active set; only a valid (or missing)
// file that lacks the active id moves visuals_set to 0.
//
// The service replaces the file with os.replace(), which swaps the inode, so
// a watch on the file alone goes blind after the first save; the directory
// is watched too, as SystemSettings watches the removable-media roots, and a
// 10 s poll backs both up. A reload whose bytes match the last read does
// nothing, which keeps the directory watch on $HOME cheap.
//
// adminInfo() is the Admin row's text: host, port, IPv4 address and PIN.
// The app has no other usable record of its own address (WifiSettings keeps
// the Wi-Fi one inside a status string, and never the Ethernet one), so it
// asks QNetworkInterface, on the first visible client and every 10 s while
// one is visible, like WifiSettings::setClientVisible.
//
// Soft contract with stock Mixxx, as for SystemSettings: tryInstance()
// returns nullptr when this was never constructed, and every widget that
// reads it renders an inert placeholder.
class VisualsSets : public QObject {
    Q_OBJECT
  public:
    struct Entry {
        int id;
        QString name;
    };
    // A set's knobs by their name in the file ("bars", "camMix"). Only knobs
    // whose value passed validation are present.
    using Knobs = QMap<QString, int>;

    static constexpr int kAdminPort = 7380;
    static constexpr int kDefaultPin = 1234;
    static constexpr int kPageRows = 0;
    static constexpr int kPageSetList = 1;
    static constexpr int kPagePinKeypad = 2;
    static constexpr int kPageLibrary = 3;

    VisualsSets(UserSettingsPointer pConfig, const QString& setsPath, QObject* parent = nullptr);
    ~VisualsSets() override;

    static VisualsSets* tryInstance() {
        return s_pInstance.loadAcquire();
    }
    // QDir::homePath() + "/.bitedj-visuals-sets.json".
    static QString defaultSetsPath();
    // A PIN control value as the four digits everyone shows. Anything that is
    // not a whole number from 0 to 9999 reads as kDefaultPin.
    static QString formatPin(double value);
    // The first IPv4 address of an up, running, non-loopback interface, or an
    // empty string.
    static QString currentIpv4();

    // Everything first, then the file's sets in file order. Only Everything
    // while the file is missing or unreadable.
    QList<Entry> entries() const;
    // Index into entries() of the set in force; 0 when that is Everything.
    int activeRow() const;
    QString activeName() const;
    QString adminInfo() const {
        return m_adminInfo;
    }
    // Makes entries()[row] the active set, applying its knobs even when it
    // already is, and moves visuals_page to returnPage. Out of range does
    // nothing.
    void selectRow(int row, int returnPage = kPageRows);
    // A widget showing adminInfo() registers while it is visible; the IP is
    // refreshed every 10 s while at least one is.
    void setClientVisible(QObject* client, bool visible);
    // Rereads the file now. The watcher and the poll call it too.
    void reload();

  signals:
    void entriesChanged(const QList<VisualsSets::Entry>& entries, int activeRow);
    void activeNameChanged(const QString& name);
    void adminInfoChanged(const QString& text);

  private slots:
    void onActiveChanged(double value);
    void onPinChanged(double value);
    void onPageChanged(double value);
    void onSettingsTabChanged(double value);

  private:
    enum class FileState {
        Missing,
        Unreadable,
        Valid,
    };
    struct Set {
        int id;
        QString name;
        Knobs knobs;
    };

    static bool parse(const QByteArray& bytes, QList<Set>* pSets, QString* pError);
    int activeId() const;
    const Set* findSet(int id) const;
    void applyKnobs(const Knobs& knobs);
    void rememberKnobs(int id, const Knobs& knobs);
    void fallBackToEverything(int missingId);
    void rearmWatches();
    // Binds the proxy on [SettingsTab],current once the skin has created
    // it. With force, rebuilds it even if already bound, so the watch never
    // depends on a proxy made against some earlier incarnation of the
    // control. Skin-created controls are deleted only when the main window
    // is (mixxxmainwindow.cpp:489), not on a skin switch, so today this is a
    // safety margin rather than a known failure.
    void bindSettingsTab(bool force);
    void refreshAdminInfo();
    void emitState();

    static QAtomicPointer<VisualsSets> s_pInstance;

    UserSettingsPointer m_pConfig;
    const QString m_setsPath;
    std::unique_ptr<ControlObject> m_pCoActive;
    std::unique_ptr<ControlObject> m_pCoRev;
    std::unique_ptr<ControlObject> m_pCoPage;
    std::unique_ptr<ControlProxy> m_pPinProxy;
    std::unique_ptr<ControlProxy> m_pSettingsTabProxy;

    FileState m_fileState;
    QList<Set> m_sets;
    // The last read, to tell a real edit from a rewrite of the same bytes.
    bool m_haveRead;
    bool m_lastExisted;
    QByteArray m_lastBytes;
    // The knobs last applied or first seen for the set that was active then.
    // A reload applies the active set's knobs only when they differ from
    // these, so an edit to some other set never undoes the panel's changes.
    bool m_haveKnobBaseline;
    int m_knobBaselineId;
    Knobs m_knobBaseline;

    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QTimer m_poll;
    QTimer m_ipTimer;
    QHash<QObject*, QMetaObject::Connection> m_visibleClients;
    QString m_ipv4;
    QString m_adminInfo;
};
