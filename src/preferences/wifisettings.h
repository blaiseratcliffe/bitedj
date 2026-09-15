#pragma once

#include <QAtomicPointer>
#include <QHash>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>

#include "preferences/usersettings.h"

class ControlObject;
class ControlPushButton;
class QProcess;

// One network in the in-skin Wi-Fi list, as parsed from nmcli. Declared at
// namespace scope rather than inside WifiSettings so the list widgets can hold
// rows without depending on the class that produces them.
struct WifiRow {
    QString ssid;
    int signalPercent = 0;   // 0..100
    bool secured = false;    // SECURITY non-empty and not "--"
    bool enterprise = false; // SECURITY contains "802.1X"
    bool saved = false;      // a 802-11-wireless profile whose NAME equals ssid
    bool active = false;     // IN-USE == "*"
};
Q_DECLARE_METATYPE(WifiRow)

// Bite DJ: backs the in-skin Settings -> Network sub-page, the recovery path
// for a box that has lost its Wi-Fi and has no keyboard or ssh session to fix
// it with. Owns the [Wifi],* COs the skin binds to, and carries the strings
// the skin cannot (the network list, the status line, the network being
// joined) on Qt signals alongside them, the same split SystemSettings uses for
// its USB rows, since CO transport carries doubles only.
//
// Every NetworkManager action goes through nmcli, run as the appliance user
// rather than root. That works because NetworkManager's polkit defaults allow
// the seated, active kiosk session to scan, join, disconnect, forget and toggle
// the radio. Measured, not assumed: pkcheck against the live app process
// returned OK for all five actions on bitepi (Phase 0, 2026-09-15).
//
// nmcli is only ever started with an argument list, never a shell string:
// SSIDs and passwords arrive from the air and from the keypad and must never
// be parsed by a shell. The password lives in m_password, and for the length of
// one join in nmcli's argv, which is how `device wifi connect` takes it. It is
// never put in a CO, a signal, a log line or a notification; only its length
// leaves this class.
//
// There is no background polling. The status refreshes every 10 s only while a
// Wi-Fi widget is on screen (setClientVisible), so a DJ who never opens the
// page pays for no nmcli call after the one status read at startup.
//
// Soft contract with stock Mixxx, as for SystemSettings: tryInstance() returns
// nullptr when the singleton is not constructed, and the widgets built on it
// fall back to an inert empty state.
class WifiSettings : public QObject {
    Q_OBJECT
  public:
    // [Wifi],state values. Also the numbering the skin's status widget keys on.
    static constexpr int kStateDisconnected = 0;
    static constexpr int kStateConnected = 1;
    static constexpr int kStateNoAdapter = 2;
    static constexpr int kStateRadioOff = 3;
    static constexpr int kStateNotResponding = 4;

    // [Wifi],page values, which are also the page indices of the Network
    // page's WidgetStack in settings.xml. Part of the skin contract: the
    // numbering cannot be changed on this side alone.
    static constexpr int kPageList = 0;
    static constexpr int kPagePassword = 1;
    static constexpr int kPageJoining = 2;
    static constexpr int kPageManage = 3;

    explicit WifiSettings(UserSettingsPointer pConfig);
    ~WifiSettings() override;

    static WifiSettings* tryInstance() {
        return s_pInstance.loadAcquire();
    }

    // Seed a freshly built widget.
    QList<WifiRow> rows() const {
        return m_rows;
    }
    QString statusLine() const {
        return m_statusLine;
    }
    int state() const {
        return m_state;
    }
    QString joinTarget() const {
        return m_joinTarget;
    }
    int passwordLength() const {
        return static_cast<int>(m_password.size());
    }

    // Widgets call this from showEvent/hideEvent. While at least one client is
    // visible, status refreshes every 10 s. The transition from zero visible
    // clients to one triggers one status refresh and one scan with --rescan
    // auto. A client destroyed while still registered is dropped as if it had
    // been hidden, so a widget torn down without a hideEvent cannot keep the
    // refresh timer running forever.
    void setClientVisible(const QObject* client, bool visible);

    // The tap dispatch, shared by WWifiList taps and the [Wifi],join CO on
    // page 0. Out of range: no-op. Active row: page 3 (checked first, so a
    // network the box is already on can always be managed, whatever its
    // security). Enterprise: notify ("Enterprise Wi-Fi is not supported") and
    // stay on page 0. Open or saved: start joining, page 2. New secured
    // network: page 1 with the password buffer cleared. Sets joinTarget and
    // emits joinTargetChanged in every case that changes page. Only acts on
    // page 0 and in states 0 and 1.
    void activateRow(int index);

    // Password buffer. Plaintext never leaves this class: not in a CO, not in
    // a signal, not in a log line. Capped at 63 characters (WPA-PSK maximum);
    // appends beyond the cap are ignored, and so is anything outside printable
    // ASCII, which is all a WPA passphrase may contain. Page 1 only.
    void appendPasswordChar(QChar c);
    void backspacePassword();
    // Refuses (notify, stay on page 1) when shorter than 8 characters.
    // Otherwise starts the join with the password, page 2.
    void submitJoin();
    // Page 1: clear the buffer, page 0. Page 2: kill the in-flight nmcli,
    // clean up any profile the attempt created, page 0. Page 3: page 0.
    void cancelJoin();

    // Static, process-free parsers of nmcli's terse (-t) output, public so they
    // can be unit tested without NetworkManager present.

    // Splits one terse line on unescaped ':'. Terse mode escapes ':' as "\:"
    // and '\' as "\\" inside a field; both are unescaped here.
    static QStringList splitTerseFields(const QString& line);
    // From `device wifi list` terse IN-USE,SSID,SIGNAL,SECURITY[,...] output.
    // Empty SSIDs (hidden networks) are dropped. nmcli prints one row per
    // BSSID, so rows are deduplicated by SSID, keeping the in-use row, else
    // the strongest. Ordered active first, then saved, then signal
    // descending, then SSID ascending (case-insensitive).
    static QList<WifiRow> parseWifiList(const QString& output, const QStringList& savedWifiNames);
    // From `connection show` terse NAME,TYPE[,...] output: names of
    // 802-11-wireless profiles.
    static QStringList parseSavedWifiNames(const QString& output);
    // From `device status` terse DEVICE,TYPE,STATE,CONNECTION: the first row
    // whose TYPE is exactly "wifi" (so never the wifi-p2p pseudo-device).
    struct WifiDevice {
        QString device;
        QString state;
        QString connection;
    };
    static std::optional<WifiDevice> parseWifiDevice(const QString& output);
    // From `device show` terse IP4.ADDRESS: "192.168.4.39/22" -> "192.168.4.39",
    // empty if none.
    static QString parseIpv4(const QString& output);

  signals:
    void networksChanged(const QList<WifiRow>& rows);
    void statusChanged(const QString& statusLine, int state);
    void joinTargetChanged(const QString& ssid);
    // Length only. The plaintext never leaves this class.
    void passwordChanged(int length);

  private slots:
    void onScanRequested(double value);
    void onJoinRequested(double value);
    void onCancelRequested(double value);
    void onDisconnectRequested(double value);
    void onForgetRequested(double value);
    void onRadioOnRequested(double value);
    void onOpWatchdogTimeout();

  private:
    // How one async nmcli run ended. Exactly one of these reaches the
    // callback given to runNmcli().
    struct NmcliResult {
        bool started = false;    // exec succeeded
        bool timedOut = false;   // the watchdog killed it
        bool cancelled = false;  // cancelJoin() killed it
        bool normalExit = false; // exited rather than crashed or was killed
        int exitCode = -1;
        QString out;
        QString err;

        bool succeeded() const {
            return started && !timedOut && !cancelled && normalExit && exitCode == 0;
        }
    };
    using NmcliCallback = std::function<void(const NmcliResult&)>;

    // Which nmcli command a join runs. A saved profile is brought up by name;
    // anything else is created by `device wifi connect`, with or without a
    // password.
    enum class JoinCommand {
        ConnectionUp,
        Connect,
        ConnectWithPassword,
    };

    // Starts `nmcli <args>` in a QProcess owned by this object, with a
    // watchdog that kills it after watchdogMs. Returns false, without calling
    // back, when another async op is in flight: one at a time, refused rather
    // than queued. Otherwise returns true and invokes callback exactly once,
    // possibly before this returns (a process that fails to start can report
    // it synchronously), so callers must finish every "op started" state
    // change before calling this. The in-flight slot is already free when the
    // callback runs, so a callback may start the next op.
    bool runNmcli(const QStringList& args, int watchdogMs, NmcliCallback callback);
    // The single terminating path of an async op: stops the watchdog,
    // disconnects and reaps the process, frees the slot, then calls back.
    // Ignores a process that is not the one in flight.
    void completeOp(QProcess* pProcess, const NmcliResult& result);
    // Ends the in-flight op as cancelled. No-op when nothing is in flight.
    void abortOp();

    // Synchronous status read (device, radio, IPv4). Each read is bounded at
    // 2 s, and the first one that fails short-circuits to state 4, so one
    // refresh blocks the GUI for about 2 s at worst.
    void refreshStatus();
    void applyStatus(int state, const QString& statusLine);
    bool isUsableState() const {
        return m_state == kStateDisconnected || m_state == kStateConnected;
    }

    // Both steps of a scan: saved profile names, then the network list.
    // Returns whether the scan started. State 0 and 1 only.
    bool startScan(bool explicitRescan);
    void onScanFinished(const NmcliResult& result);
    void publishRows(QList<WifiRow> rows);

    // Returns whether the join started. Refuses (notifies) while another op
    // is in flight.
    // Takes ssid by value: callers pass m_joinTarget, which this rewrites.
    bool startJoin(QString ssid, JoinCommand command);
    void onJoinFinished(const QString& ssid, const NmcliResult& result);
    // Deletes, asynchronously, a profile named exactly ssid if the join
    // snapshot shows it did not exist before the attempt. Consumes the
    // snapshot. Never deletes a profile that was there before.
    void cleanupResidue(const QString& ssid);

    bool startDisconnect();
    bool startForget();
    bool startRadioOn();

    int page() const;
    void setPage(int page);
    // Emits joinTargetChanged unconditionally: the widgets repaint on it, and
    // a page change back to the same target still has to reach them.
    void setJoinTarget(const QString& ssid);
    void clearPassword();
    // Clears the password and the join target and returns to page 0.
    void goToList();
    // [Wifi],selected_index as a row index, or -1 when it names no row.
    int selectedIndex() const;
    // Human-readable reason for a failed op, with the password (if any is in
    // the buffer) masked out in case nmcli ever echoes it.
    QString describeFailure(const NmcliResult& result) const;
    void notifyBusy();

    static QAtomicPointer<WifiSettings> s_pInstance;

    // Unused today. Kept so the constructor has the same shape as its sibling
    // settings singletons.
    UserSettingsPointer m_pConfig;

    QList<WifiRow> m_rows;
    // Names of the saved wifi profiles, from the last scan. What each row's
    // saved flag is computed against.
    QStringList m_savedWifiNames;
    QString m_statusLine;
    int m_state = kStateDisconnected;
    // Interface name from `device status` (wlan0 on bitepi), never hardcoded.
    // Empty when there is no wifi device.
    QString m_wifiDevice;
    QString m_joinTarget;
    QString m_password;

    // Every profile name (any type) seen just before the current join, so a
    // failure can tell a profile the attempt created from one that was already
    // there. Absent when that read failed, in which case nothing is ever
    // deleted: leaving residue is recoverable, deleting a DJ's profile is not.
    std::optional<QStringList> m_joinSnapshot;

    // The async op slot. m_opInFlight is the guard; m_pOpProcess the process
    // (a child of this object) and m_opCallback its one-shot continuation.
    bool m_opInFlight = false;
    QProcess* m_pOpProcess = nullptr;
    NmcliCallback m_opCallback;
    // Whether the op in flight is a join, which is the only op cancelJoin()
    // kills. Also what tells onJoinRequested that the tap started something
    // that now owns the [Wifi],join CO until it ends.
    bool m_joinInFlight = false;
    QTimer m_opWatchdog;

    // Widgets currently on screen, each with the connection that drops it if
    // it is destroyed without being hidden first.
    QHash<const QObject*, QMetaObject::Connection> m_visibleClients;
    QTimer m_statusTimer;

    std::unique_ptr<ControlObject> m_pCoState;
    // Must stay a plain ControlObject, for the same reason as
    // [System],power_arm: a ControlPushButton is built around a fixed state
    // count and is the wrong shape for a 0..3 page index.
    std::unique_ptr<ControlObject> m_pCoPage;
    std::unique_ptr<ControlObject> m_pCoNetworkCount;
    std::unique_ptr<ControlObject> m_pCoSelectedIndex;
    // Action COs. Each is put back to 0 on every path that ends the action it
    // started (success, failure, refusal, no-op), the rule
    // SystemSettings::resetPowerControls documents: ControlDoublePrivate::
    // setInner drops a write of the value a CO already holds (control.cpp
    // :293-296), so a CO left latched at 1 turns the next tap into a silent
    // dead one. A ControlPushButton happens to be built with that no-op check
    // off, so today a second 1 would still get through; the reset is kept
    // regardless, so the rule does not hinge on which CO class a key is, and
    // so the value read back over the control socket is truthful: 1 while the
    // action runs (which also lets the skin light the button as progress), 0
    // once it has ended.
    std::unique_ptr<ControlPushButton> m_pCoScan;
    std::unique_ptr<ControlPushButton> m_pCoJoin;
    std::unique_ptr<ControlPushButton> m_pCoCancel;
    std::unique_ptr<ControlPushButton> m_pCoDisconnect;
    std::unique_ptr<ControlPushButton> m_pCoForget;
    std::unique_ptr<ControlPushButton> m_pCoRadioOn;
};
