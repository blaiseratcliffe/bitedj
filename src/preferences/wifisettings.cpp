#include "preferences/wifisettings.h"

#include <QElapsedTimer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QtDebug>
#include <algorithm>
#include <utility>

#include "control/controlobject.h"
#include "control/controlpushbutton.h"
#include "moc_wifisettings.cpp"
#include "notifications/notifications.h"
#include "util/fpclassify.h"

namespace {
const QString kGroup = QStringLiteral("[Wifi]");
const QString kNmcli = QStringLiteral("nmcli");

// Status refresh period while a Wi-Fi widget is on screen. Often enough that a
// network joined or lost behind the DJ's back shows up while they are looking
// at the page, and never at all while they are not.
constexpr int kStatusRefreshMs = 10000;

// Bound on a synchronous read that stands on its own (the join snapshot).
// nmcli answers a local D-Bus query in tens of milliseconds, so this only
// fires when NetworkManager is wedged, and it is how long the GUI thread
// stalls when it is, plus up to kReapTimeoutMs reaping the nmcli it then
// kills: about 3 s in all. Deliberately not the 30 s default of
// QProcess::waitForFinished(), which the tryUnmount() pattern this is modelled
// on still uses.
constexpr int kSyncReadTimeoutMs = 2000;

// Bound on a whole status refresh, shared by its three reads rather than
// granted to each. Reads that fail short-circuit, but reads that merely answer
// slowly do not, so three of them at 2 s each would stall the GUI thread for
// 6 s while the code and the contract both promised about 2. The worst case
// is this plus one reap (kReapTimeoutMs) of the read that ran out: about 3 s.
constexpr int kStatusBudgetMs = 2000;
// Floor on what is left of that budget when a read starts. A read handed a
// millisecond is a read that cannot succeed, so the budget is allowed to
// overrun by this much rather than turn a working NetworkManager into state 4.
constexpr int kMinSyncReadTimeoutMs = 100;

// How long to wait for a process we have just SIGKILLed to be reaped, so its
// QProcess is never destroyed while still running (which warns, then blocks).
constexpr int kReapTimeoutMs = 1000;

// Watchdogs on the async ops. Each is longer than the --wait nmcli is given,
// so normally nmcli gives up first and says why; the watchdog is only for an
// nmcli that has stopped answering altogether.
constexpr int kScanWatchdogMs = 20000;
constexpr int kConnectWatchdogMs = 40000;
constexpr int kDisconnectWatchdogMs = 15000; // also connection delete
constexpr int kRadioWatchdogMs = 10000;

// How long after a successful `radio wifi on` to look again. rfkill and the
// supplicant do not come up the instant nmcli returns, so the refresh that
// follows the command can still read the radio as off and leave "Wi-Fi is off"
// on a screen whose radio is already coming on, inviting a second tap.
constexpr int kRadioSettleMs = 500;
const QString kConnectWaitSeconds = QStringLiteral("30");
const QString kShortWaitSeconds = QStringLiteral("10");

// WPA-PSK passphrase bounds (IEEE 802.11i): 8 to 63 printable ASCII
// characters. NetworkManager rejects anything outside them with a message
// about the psk property, which would read here like a wrong password.
constexpr int kMinPasswordLength = 8;
constexpr int kMaxPasswordLength = 63;

void notify(const QString& message, Notifications::Severity severity) {
    if (Notifications* pNotifications = Notifications::tryInstance()) {
        pNotifications->publish(message, severity);
    }
}

// nmcli translates its messages and some field values (device STATE, the
// radio switch) into the session's language, and a join's outcome is decided
// by matching its English stderr. C.UTF-8 switches translation off without
// switching UTF-8 off: under plain C, glib converts output to ASCII on its way
// to stdout, and a non-ASCII SSID would come back as a name no network has.
QProcessEnvironment nmcliEnvironment() {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
    // GNU gettext lets LANGUAGE outrank LC_ALL for message catalogs unless the
    // locale is exactly "C", which C.UTF-8 is not.
    environment.remove(QStringLiteral("LANGUAGE"));
    return environment;
}

QStringList outputLines(const QString& output) {
    QStringList lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (QString& line : lines) {
        if (line.endsWith(QLatin1Char('\r'))) {
            line.chop(1);
        }
    }
    return lines;
}

// Every profile name in `connection show` terse NAME,TYPE output, whatever its
// type. The residue check needs all of them, not just the wifi ones:
// `connection delete id` matches a name across every type.
QStringList allProfileNames(const QString& output) {
    QStringList names;
    const QStringList lines = outputLines(output);
    for (const QString& line : lines) {
        const QString name = WifiSettings::splitTerseFields(line).value(0);
        if (!name.isEmpty()) {
            names.append(name);
        }
    }
    return names;
}

// Row order the list is shown in. SSIDs are unique after deduplication, so the
// case-sensitive tie-break makes this a total order and the result
// deterministic.
bool rowLess(const WifiRow& a, const WifiRow& b) {
    if (a.active != b.active) {
        return a.active;
    }
    if (a.saved != b.saved) {
        return a.saved;
    }
    if (a.signalPercent != b.signalPercent) {
        return a.signalPercent > b.signalPercent;
    }
    const int caseless = a.ssid.compare(b.ssid, Qt::CaseInsensitive);
    if (caseless != 0) {
        return caseless < 0;
    }
    return a.ssid < b.ssid;
}
} // namespace

QAtomicPointer<WifiSettings> WifiSettings::s_pInstance = nullptr;
WifiSettings::NmcliFake* WifiSettings::s_pNmcliFake = nullptr;

// static
// The bound is a parameter because the three reads of a status refresh share
// one budget between them.
bool WifiSettings::readNmcliSync(
        const QStringList& args, int timeoutMs, QString* pOut, QString* pError) {
    if (s_pNmcliFake) {
        return s_pNmcliFake->readSync(args, timeoutMs, pOut, pError);
    }
    QProcess process;
    process.setProcessEnvironment(nmcliEnvironment());
    // No stdin: nmcli must never sit waiting on a prompt for input that
    // cannot come.
    process.setStandardInputFile(QProcess::nullDevice());
    process.start(kNmcli, args);
    if (!process.waitForFinished(timeoutMs)) {
        QString error;
        if (process.error() == QProcess::FailedToStart) {
            error = QStringLiteral("could not run nmcli: ") + process.errorString();
        } else {
            error = QStringLiteral("nmcli did not answer within %1 ms").arg(timeoutMs);
        }
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(kReapTimeoutMs);
        }
        if (pError) {
            *pError = error;
        }
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        QString error = QString::fromUtf8(process.readAllStandardError()).simplified();
        if (error.isEmpty()) {
            error = QStringLiteral("nmcli exited with code %1").arg(process.exitCode());
        }
        if (pError) {
            *pError = error;
        }
        return false;
    }
    if (pOut) {
        *pOut = QString::fromUtf8(process.readAllStandardOutput());
    }
    return true;
}

WifiSettings::WifiSettings(UserSettingsPointer pConfig)
        : m_pConfig(std::move(pConfig)),
          m_statusLine(tr("Not connected")) {
    m_pCoState = std::make_unique<ControlObject>(ConfigKey(kGroup, QStringLiteral("state")));
    m_pCoState->setReadOnly();

    // Pre-created, as [System],power_arm is, so the skin parser's
    // controlFromConfigKey() reuses this object for the Network page's
    // WidgetStack and it has a value to read on its first showEvent.
    m_pCoPage = std::make_unique<ControlObject>(ConfigKey(kGroup, QStringLiteral("page")));

    m_pCoNetworkCount = std::make_unique<ControlObject>(
            ConfigKey(kGroup, QStringLiteral("network_count")));
    m_pCoNetworkCount->setReadOnly();

    m_pCoSelectedIndex = std::make_unique<ControlObject>(
            ConfigKey(kGroup, QStringLiteral("selected_index")));

    // The action COs, each pre-created (so the skin binds to this object, not
    // one the parser invents) and wired to the slot that acts on its 1.
    const auto makeAction = [this](const QString& item,
                                    void (WifiSettings::*onRequested)(double)) {
        auto pCo = std::make_unique<ControlPushButton>(ConfigKey(kGroup, item));
        connect(pCo.get(), &ControlObject::valueChanged, this, onRequested);
        return pCo;
    };
    m_pCoScan = makeAction(QStringLiteral("scan"), &WifiSettings::onScanRequested);
    m_pCoJoin = makeAction(QStringLiteral("join"), &WifiSettings::onJoinRequested);
    m_pCoCancel = makeAction(QStringLiteral("cancel"), &WifiSettings::onCancelRequested);
    m_pCoDisconnect =
            makeAction(QStringLiteral("disconnect"), &WifiSettings::onDisconnectRequested);
    m_pCoForget = makeAction(QStringLiteral("forget"), &WifiSettings::onForgetRequested);
    m_pCoRadioOn = makeAction(QStringLiteral("radio_on"), &WifiSettings::onRadioOnRequested);

    m_opWatchdog.setSingleShot(true);
    connect(&m_opWatchdog, &QTimer::timeout, this, &WifiSettings::onOpWatchdogTimeout);

    m_statusTimer.setSingleShot(false);
    m_statusTimer.setInterval(kStatusRefreshMs);
    connect(&m_statusTimer, &QTimer::timeout, this, [this]() {
        refreshStatus();
        // Brings back a scan that was lost to a busy slot or a pre-emption,
        // with the connection unchanged, by this tick at the latest.
        rescanIfRowsStale();
    });

    s_pInstance.storeRelease(this);

    // No nmcli in the constructor: this runs on the way to the first frame.
    // One deferred status read instead, so [Wifi],state is truthful soon after
    // startup, before any Wi-Fi widget exists, for the control socket as much
    // as for the skin.
    QTimer::singleShot(0, this, &WifiSettings::refreshStatus);
}

WifiSettings::~WifiSettings() {
    s_pInstance.storeRelease(nullptr);
    m_statusTimer.stop();
    m_opWatchdog.stop();
    for (const QMetaObject::Connection& connection : std::as_const(m_visibleClients)) {
        QObject::disconnect(connection);
    }
    m_visibleClients.clear();

    // Drop the continuation before touching the process: it captures this,
    // and must never run into an object that is half torn down. A join killed
    // here is not cleaned up; there is no object left to own the delete.
    m_opCallback = nullptr;
    if (m_pOpProcess) {
        m_pOpProcess->disconnect(this);
        if (m_pOpProcess->state() != QProcess::NotRunning) {
            m_pOpProcess->kill();
            m_pOpProcess->waitForFinished(kReapTimeoutMs);
        }
        // Deleted with this object's other children.
        m_pOpProcess = nullptr;
    }
    m_password.clear();
}

void WifiSettings::setClientVisible(const QObject* client, bool visible) {
    if (!client) {
        return;
    }
    const bool wasAnyVisible = !m_visibleClients.isEmpty();
    if (visible) {
        if (m_visibleClients.contains(client)) {
            return;
        }
        m_visibleClients.insert(client,
                connect(client, &QObject::destroyed, this, [this, client]() {
                    setClientVisible(client, false);
                }));
    } else {
        const auto it = m_visibleClients.find(client);
        if (it == m_visibleClients.end()) {
            return;
        }
        QObject::disconnect(it.value());
        m_visibleClients.erase(it);
    }
    const bool isAnyVisible = !m_visibleClients.isEmpty();
    if (!wasAnyVisible && isAnyVisible) {
        m_statusTimer.start();
        refreshStatus();
        startScan(false);
    } else if (wasAnyVisible && !isAnyVisible) {
        m_statusTimer.stop();
    }
}

void WifiSettings::activateRow(int index) {
    if (!isUsableState() || page() != kPageList || index < 0 || index >= m_rows.size()) {
        return;
    }
    const WifiRow row = m_rows.at(index);
    // Keep the CO in step with a tap that came straight from the list widget,
    // so a later [Wifi],join acts on the same row.
    m_pCoSelectedIndex->set(static_cast<double>(index));

    // Not row.active alone: after a dropped link or a switch to another
    // network the flag is stale until the rescan lands, and page 3 for a
    // network the box has left would disconnect whatever it is on now.
    if (isActiveNow(row)) {
        setJoinTarget(row.ssid);
        setPage(kPageManage);
        return;
    }
    if (row.enterprise) {
        notify(tr("Enterprise Wi-Fi is not supported"), Notifications::Severity::Warning);
        return;
    }
    if (row.saved) {
        // Brought up by the profile's UUID, not by the SSID: the profile can
        // be called anything. With two profiles for one SSID, the first one
        // nmcli listed. A row flagged saved whose profile has since gone
        // (a scan's profile steps landed, its list step failed) falls through
        // and is joined as the unsaved network it now is.
        if (const SavedProfile* pProfile = savedProfileForSsid(row.ssid)) {
            startJoin(JoinTarget{row.ssid, pProfile->name, pProfile->uuid},
                    JoinCommand::ConnectionUp);
            return;
        }
    }
    if (!row.secured) {
        startJoin(JoinTarget{row.ssid, QString(), QString()}, JoinCommand::Connect);
        return;
    }
    clearPassword();
    setJoinTarget(row.ssid);
    setPage(kPagePassword);
}

void WifiSettings::appendPasswordChar(QChar c) {
    if (page() != kPagePassword) {
        return;
    }
    // Printable ASCII only, which is all a WPA passphrase may hold. Also keeps
    // a NUL, which would truncate the argument nmcli receives, out of argv.
    if (c.unicode() < 0x20 || c.unicode() > 0x7E) {
        return;
    }
    if (m_password.size() >= kMaxPasswordLength) {
        return;
    }
    m_password.append(c);
    emit passwordChanged(passwordLength());
}

void WifiSettings::backspacePassword() {
    if (page() != kPagePassword || m_password.isEmpty()) {
        return;
    }
    m_password.chop(1);
    emit passwordChanged(passwordLength());
}

void WifiSettings::submitJoin() {
    if (!isUsableState() || page() != kPagePassword || m_joinTarget.isEmpty()) {
        return;
    }
    if (m_password.size() < kMinPasswordLength) {
        notify(tr("Password must be at least %1 characters").arg(kMinPasswordLength),
                Notifications::Severity::Warning);
        return;
    }
    startJoin(JoinTarget{m_joinTarget, QString(), QString()}, JoinCommand::ConnectWithPassword);
}

void WifiSettings::cancelJoin() {
    switch (page()) {
    case kPagePassword:
    case kPageManage:
        goToList();
        break;
    case kPageJoining:
        if (m_joinInFlight) {
            // onJoinFinished() sees the cancel, cleans up any profile the
            // attempt created and returns to the list.
            abortOp();
        } else {
            goToList();
        }
        break;
    default:
        break;
    }
}

// static
QStringList WifiSettings::splitTerseFields(const QString& line) {
    QStringList fields;
    QString field;
    for (qsizetype i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (c == QLatin1Char('\\') && i + 1 < line.size()) {
            // An escape: whatever follows is literal, ':' and '\' included.
            ++i;
            field.append(line.at(i));
        } else if (c == QLatin1Char(':')) {
            fields.append(field);
            field.clear();
        } else {
            field.append(c);
        }
    }
    fields.append(field);
    return fields;
}

// static
QList<WifiRow> WifiSettings::parseWifiList(
        const QString& output, const QStringList& savedSsids) {
    QList<WifiRow> rows;
    const QStringList lines = outputLines(output);
    for (const QString& line : lines) {
        const QStringList fields = splitTerseFields(line);
        if (fields.size() < 4) {
            continue;
        }
        WifiRow row;
        // Not trimmed: spaces are legal in an SSID, and terse mode pads nothing.
        row.ssid = fields.at(1);
        if (row.ssid.isEmpty()) {
            // A hidden network. Nothing to show and nothing to join by.
            continue;
        }
        bool signalOk = false;
        const int signalValue = fields.at(2).trimmed().toInt(&signalOk);
        row.signalPercent = signalOk ? std::clamp(signalValue, 0, 100) : 0;
        const QString security = fields.at(3).trimmed();
        row.secured = !security.isEmpty() && security != QLatin1String("--");
        row.enterprise = security.contains(QLatin1String("802.1X"));
        row.saved = savedSsids.contains(row.ssid);
        row.active = fields.at(0).trimmed() == QLatin1String("*");

        // One row per BSSID from nmcli; one per network on screen.
        const auto existing = std::find_if(rows.begin(), rows.end(), [&row](const WifiRow& other) {
            return other.ssid == row.ssid;
        });
        if (existing == rows.end()) {
            rows.append(row);
        } else if (!existing->active &&
                (row.active || row.signalPercent > existing->signalPercent)) {
            *existing = row;
        }
    }
    std::sort(rows.begin(), rows.end(), rowLess);
    return rows;
}

// static
QList<WifiSettings::SavedProfile> WifiSettings::parseWifiProfileList(const QString& output) {
    QList<SavedProfile> profiles;
    const QStringList lines = outputLines(output);
    for (const QString& line : lines) {
        const QStringList fields = splitTerseFields(line);
        if (fields.size() < 3 || fields.at(0).isEmpty() || fields.at(1).isEmpty()) {
            continue;
        }
        if (fields.at(2) != QLatin1String("802-11-wireless")) {
            continue;
        }
        // A UUID is unique by construction; a repeat can only be the same
        // profile listed twice.
        const QString uuid = fields.at(1);
        const bool seen = std::any_of(profiles.cbegin(),
                profiles.cend(),
                [&uuid](const SavedProfile& profile) { return profile.uuid == uuid; });
        if (!seen) {
            profiles.append(SavedProfile{fields.at(0), uuid, QString()});
        }
    }
    return profiles;
}

// static
QList<WifiSettings::SavedProfile> WifiSettings::parseSavedWifiProfiles(const QString& output) {
    QList<SavedProfile> profiles;
    // The block being read, and which of its keys have been seen. A key seen
    // twice also ends a block, so a missing separator cannot merge two
    // profiles into one.
    SavedProfile block;
    bool hasName = false;
    bool hasUuid = false;
    bool hasSsid = false;
    const auto endBlock = [&]() {
        // Keyed on the UUID the block itself names, never on its position
        // among the uuids asked for. No SSID line: not a Wi-Fi profile.
        if (hasUuid && hasSsid && !block.uuid.isEmpty() && !block.ssid.isEmpty()) {
            const QString& uuid = block.uuid;
            const bool seen = std::any_of(profiles.cbegin(),
                    profiles.cend(),
                    [&uuid](const SavedProfile& profile) { return profile.uuid == uuid; });
            if (!seen) {
                profiles.append(block);
            }
        }
        block = SavedProfile();
        hasName = false;
        hasUuid = false;
        hasSsid = false;
    };

    // Not outputLines(), which drops the empty lines that separate blocks.
    const QStringList lines = output.split(QLatin1Char('\n'));
    for (QString line : lines) {
        if (line.endsWith(QLatin1Char('\r'))) {
            line.chop(1);
        }
        if (line.isEmpty()) {
            endBlock();
            continue;
        }
        // Values are escaped like any other terse field, so an SSID holding
        // ':' or '\' comes back whole.
        const QStringList fields = splitTerseFields(line);
        if (fields.size() < 2) {
            continue;
        }
        const QString& key = fields.at(0);
        const QString& value = fields.at(1);
        if (key == QLatin1String("connection.id")) {
            if (hasName) {
                endBlock();
            }
            block.name = value;
            hasName = true;
        } else if (key == QLatin1String("connection.uuid")) {
            if (hasUuid) {
                endBlock();
            }
            block.uuid = value;
            hasUuid = true;
        } else if (key == QLatin1String("802-11-wireless.ssid")) {
            if (hasSsid) {
                endBlock();
            }
            block.ssid = value;
            hasSsid = true;
        }
    }
    // nmcli prints no blank line after the last block.
    endBlock();
    return profiles;
}

// static
std::optional<WifiSettings::WifiDevice> WifiSettings::parseWifiDevice(const QString& output) {
    const QStringList lines = outputLines(output);
    for (const QString& line : lines) {
        const QStringList fields = splitTerseFields(line);
        // Exactly "wifi": the p2p-dev-wlan0 pseudo-device is "wifi-p2p" and
        // cannot join anything.
        if (fields.size() < 2 || fields.at(1) != QLatin1String("wifi") ||
                fields.at(0).isEmpty()) {
            continue;
        }
        WifiDevice device;
        device.device = fields.at(0);
        device.state = fields.value(2);
        device.connection = fields.value(3);
        return device;
    }
    return std::nullopt;
}

// static
QString WifiSettings::parseActiveProfile(const QString& output, const QString& device) {
    if (device.isEmpty()) {
        return QString();
    }
    const QStringList lines = outputLines(output);
    for (const QString& line : lines) {
        const QStringList fields = splitTerseFields(line);
        if (fields.size() >= 3 && fields.at(2) == device && !fields.at(0).isEmpty()) {
            return fields.at(0);
        }
    }
    return QString();
}

// static
bool WifiSettings::isWrongPasswordError(const QString& nmcliError) {
    // "Secrets were required" is NetworkManager's own activation failure for a
    // rejected passphrase; newer nmcli words the same failure "Passwords or
    // encryption keys are required"; a passphrase NM refuses outright is
    // reported against the full property name.
    return nmcliError.contains(QLatin1String("Secrets were required"), Qt::CaseInsensitive) ||
            nmcliError.contains(QLatin1String("Passwords or encryption keys are required"),
                    Qt::CaseInsensitive) ||
            nmcliError.contains(QLatin1String("802-11-wireless-security.psk"),
                    Qt::CaseInsensitive);
}

// static
QString WifiSettings::parseIpv4(const QString& output) {
    const QStringList lines = outputLines(output);
    for (const QString& line : lines) {
        const QStringList fields = splitTerseFields(line);
        if (fields.size() < 2 || !fields.at(0).startsWith(QLatin1String("IP4.ADDRESS"))) {
            continue;
        }
        QString address = fields.at(1).trimmed();
        const qsizetype slash = address.indexOf(QLatin1Char('/'));
        if (slash >= 0) {
            address.truncate(slash);
        }
        if (!address.isEmpty()) {
            return address;
        }
    }
    return QString();
}

// static
QString WifiSettings::parseConnectionUuid(const QString& output) {
    const QStringList lines = outputLines(output);
    for (const QString& line : lines) {
        const QStringList fields = splitTerseFields(line);
        if (fields.size() >= 2 && fields.at(0) == QLatin1String("GENERAL.CON-UUID")) {
            return fields.at(1).trimmed();
        }
    }
    return QString();
}

void WifiSettings::onScanRequested(double value) {
    if (value == 0.0) {
        return;
    }
    // In every state, not just 0 and 1 (Ruling 16): the refresh is the only
    // way out of state 2, 3 or 4 when no widget is on screen to run the 10 s
    // timer, so without it one startup blip would leave state 4 for good. It
    // then scans only if the refresh made the state usable.
    refreshStatus();
    if (startScan(true)) {
        // onScanFinished() puts the CO back when the scan ends.
        return;
    }
    // Refused because a scan already holds the slot, typically the auto scan
    // the refresh just started on seeing a change: that one is the answer to
    // this tap, and its own onScanFinished() resets the CO, so it stays 1 for
    // as long as a scan is actually running.
    if (m_scanInFlight) {
        return;
    }
    m_pCoScan->set(0.0);
}

void WifiSettings::onJoinRequested(double value) {
    if (value == 0.0) {
        return;
    }
    const bool joinWasInFlight = m_joinInFlight;
    if (isUsableState()) {
        const int currentPage = page();
        if (currentPage == kPageList) {
            activateRow(selectedIndex());
        } else if (currentPage == kPagePassword) {
            submitJoin();
        }
    }
    // A join this tap started owns the CO until it ends (onJoinFinished()
    // resets it). Every other outcome, a tap refused while an earlier join
    // runs included, has already ended here.
    if (joinWasInFlight || !m_joinInFlight) {
        m_pCoJoin->set(0.0);
    }
}

void WifiSettings::onCancelRequested(double value) {
    if (value == 0.0) {
        return;
    }
    // Deliberately not gated on state, unlike the actions. Backing out is
    // always safe, and a join in flight when NetworkManager stops answering
    // (state 4, page 2) would otherwise hold the DJ until its 40 s watchdog.
    cancelJoin();
    m_pCoCancel->set(0.0);
}

void WifiSettings::onDisconnectRequested(double value) {
    if (value == 0.0) {
        return;
    }
    if (!startDisconnect()) {
        m_pCoDisconnect->set(0.0);
    }
}

void WifiSettings::onForgetRequested(double value) {
    if (value == 0.0) {
        return;
    }
    if (!startForget()) {
        m_pCoForget->set(0.0);
    }
}

void WifiSettings::onRadioOnRequested(double value) {
    if (value == 0.0) {
        return;
    }
    if (!startRadioOn()) {
        m_pCoRadioOn->set(0.0);
    }
}

void WifiSettings::onOpWatchdogTimeout() {
    if (!m_opInFlight) {
        return;
    }
    // Info, not warning: every caller reports its own failure, and one fault
    // should be one line in the log.
    qInfo() << "WifiSettings: nmcli outlived its watchdog, killing it";
    NmcliResult result;
    result.started = true;
    result.timedOut = true;
    endOp(result);
}

bool WifiSettings::runNmcli(const QStringList& args, int watchdogMs, NmcliCallback callback) {
    if (m_opInFlight) {
        return false;
    }
    m_opInFlight = true;
    m_opCallback = std::move(callback);

    if (s_pNmcliFake) {
        // The test seam: no process, and the op stays in flight until the
        // test ends it through finishOp(). Armed like the real thing, though
        // with no event loop turning in that harness it never fires.
        m_opWatchdog.start(watchdogMs);
        if (!s_pNmcliFake->startAsync(args)) {
            // A call the fake was not told to expect. It has already failed
            // the test; ending the op as one that never started keeps
            // whatever the code does next as close to a failed exec as
            // possible, rather than leaving it waiting forever.
            NmcliResult result;
            result.err = QStringLiteral("nmcli call not scripted by the test");
            finishOp(result);
        }
        return true;
    }

    // Owned, never startDetached, for the reason SystemSettings::
    // runPowerCommand gives: a polkit refusal is a non-zero exit after a
    // perfectly good exec, which only an owned process can see. Here success
    // also has to hand stdout to a parser.
    auto* pProcess = new QProcess(this);
    m_pOpProcess = pProcess;
    pProcess->setProcessEnvironment(nmcliEnvironment());
    // No stdin: a join whose secrets NetworkManager wants asked for must fail
    // and say so, not wait on a prompt until the watchdog.
    pProcess->setStandardInputFile(QProcess::nullDevice());

    // finished and errorOccurred are both needed: a process that fails to
    // start never emits finished. Whichever arrives first ends the op, and
    // completeOp() disconnects the other.
    connect(pProcess,
            &QProcess::finished,
            this,
            [this, pProcess](int exitCode, QProcess::ExitStatus exitStatus) {
                NmcliResult result;
                result.started = true;
                result.normalExit = exitStatus == QProcess::NormalExit;
                result.exitCode = exitCode;
                result.out = QString::fromUtf8(pProcess->readAllStandardOutput());
                result.err = QString::fromUtf8(pProcess->readAllStandardError());
                completeOp(pProcess, result);
            });
    connect(pProcess,
            &QProcess::errorOccurred,
            this,
            [this, pProcess](QProcess::ProcessError error) {
                NmcliResult result;
                result.started = error != QProcess::FailedToStart;
                if (result.started) {
                    result.out = QString::fromUtf8(pProcess->readAllStandardOutput());
                    result.err = QString::fromUtf8(pProcess->readAllStandardError());
                } else {
                    result.err = pProcess->errorString();
                }
                completeOp(pProcess, result);
            });

    // Armed before start(), which can report a failure synchronously and so
    // complete the op, and stop this timer, before it returns.
    m_opWatchdog.start(watchdogMs);
    pProcess->start(kNmcli, args);
    return true;
}

void WifiSettings::completeOp(QProcess* pProcess, const NmcliResult& result) {
    if (!pProcess || pProcess != m_pOpProcess) {
        return;
    }
    m_opWatchdog.stop();
    // Disconnect first, so that neither the other of finished/errorOccurred
    // nor the finished that waitForFinished() below emits synchronously can
    // report this op a second time.
    pProcess->disconnect(this);
    if (pProcess->state() != QProcess::NotRunning) {
        pProcess->kill();
        pProcess->waitForFinished(kReapTimeoutMs);
    }
    pProcess->deleteLater();
    m_pOpProcess = nullptr;
    finishOp(result);
}

void WifiSettings::finishOp(const NmcliResult& result) {
    m_opWatchdog.stop();
    m_opInFlight = false;
    // Moved out before the call: the callback may start the next op, which
    // installs a callback of its own.
    NmcliCallback callback = std::move(m_opCallback);
    m_opCallback = nullptr;
    if (callback) {
        callback(result);
    }
}

void WifiSettings::endOp(const NmcliResult& result) {
    if (m_pOpProcess) {
        completeOp(m_pOpProcess, result);
    } else if (m_opInFlight) {
        // Only the test seam holds the slot with no process behind it.
        finishOp(result);
    }
}

void WifiSettings::abortOp() {
    NmcliResult result;
    result.started = true;
    result.cancelled = true;
    endOp(result);
}

bool WifiSettings::claimOpSlot() {
    if (!m_opInFlight) {
        return true;
    }
    if (!m_scanInFlight) {
        notifyBusy();
        return false;
    }
    // onScanFinished() sees the cancel, resets [Wifi],scan and drops the
    // result; it starts nothing, so the slot is free when this returns.
    qInfo() << "WifiSettings: pre-empting the scan in flight for a user action";
    abortOp();
    return !m_opInFlight;
}

void WifiSettings::refreshStatus() {
    QString output;
    QString error;
    const auto reportNotResponding = [this, &error]() {
        if (m_state != kStateNotResponding) {
            qWarning() << "WifiSettings: NetworkManager is not responding:" << error;
        }
        applyStatus(kStateNotResponding);
    };

    // One refresh is up to three reads in a row on the GUI thread, and they
    // share kStatusBudgetMs between them. Per-read bounds would only have
    // bounded the failing case: three reads that each answer slowly never
    // short-circuit, and each would have been entitled to its own 2 s.
    QElapsedTimer budget;
    budget.start();
    const auto readWithinBudget = [&budget, &error](const QStringList& args, QString* pOut) {
        const qint64 left = kStatusBudgetMs - budget.elapsed();
        if (left <= 0) {
            // A spent budget is a read that failed: same short-circuit, same
            // state 4, so the bound holds however the reads before it ended.
            error = QStringLiteral("the status refresh spent its %1 ms budget")
                            .arg(kStatusBudgetMs);
            return false;
        }
        return readNmcliSync(args,
                static_cast<int>(std::max<qint64>(left, kMinSyncReadTimeoutMs)),
                pOut,
                &error);
    };

    if (!readWithinBudget({"-t", "-f", "DEVICE,TYPE,STATE,CONNECTION", "device", "status"},
                &output)) {
        reportNotResponding();
        return;
    }
    const std::optional<WifiDevice> device = parseWifiDevice(output);
    if (!device) {
        m_wifiDevice.clear();
        applyStatus(kStateNoAdapter);
        return;
    }
    m_wifiDevice = device->device;

    // The software radio switch, the one `nmcli radio wifi on` flips. The
    // exact form measured on bitepi in Phase 0.
    if (!readWithinBudget({"-t", "-f", "WIFI", "general"}, &output)) {
        reportNotResponding();
        return;
    }
    if (output.trimmed() != QLatin1String("enabled")) {
        applyStatus(kStateRadioOff);
        return;
    }

    // Covers "connected" and "connected (externally)". Everything else,
    // including the "connecting (...)" steps of a join in progress, is not
    // connected yet.
    if (!device->state.startsWith(QLatin1String("connected"))) {
        applyStatus(kStateDisconnected);
        return;
    }
    // The last read, so a failure here has nothing left to short-circuit:
    // report the link without an address rather than call a working link dead.
    // The same read names the active connection by UUID, which, unlike its
    // name, cannot be shared by two profiles.
    QString ipv4;
    QString uuid;
    if (readWithinBudget({"-t", "-f", "IP4.ADDRESS,GENERAL.CON-UUID", "device", "show",
                                 m_wifiDevice},
                &output)) {
        ipv4 = parseIpv4(output);
        uuid = parseConnectionUuid(output);
    }
    applyStatus(kStateConnected, device->connection, ipv4, uuid);
}

void WifiSettings::applyStatus(
        int state, const QString& connection, const QString& ipv4, const QString& uuid) {
    const int oldState = m_state;
    const QString oldConnection = m_connectedProfile;
    const QString oldUuid = m_connectedUuid;
    m_state = state;
    m_connectedProfile = state == kStateConnected ? connection : QString();
    m_connectedIpv4 = state == kStateConnected ? ipv4 : QString();
    m_connectedUuid = state == kStateConnected ? uuid : QString();
    const bool stateChanged = state != oldState;
    if (stateChanged) {
        m_pCoState->forceSet(static_cast<double>(state));
    }
    updateStatusLine(stateChanged);

    // Covers a dropped link (1 -> 0) and a move from one network to another
    // (1 -> 1), which is not a state change at all, including a move between
    // two profiles that share a name, which only the UUID shows. A UUID that
    // is merely unknown this time (its read failed) is not a move: that would
    // throw page 3 away on one slow read.
    const bool uuidChanged =
            !oldUuid.isEmpty() && !m_connectedUuid.isEmpty() && m_connectedUuid != oldUuid;
    const bool connectionChanged = m_connectedProfile != oldConnection || uuidChanged;
    if (!stateChanged && !connectionChanged) {
        return;
    }

    if (!isUsableState()) {
        // No adapter, radio off or no answer: the list means nothing, and
        // nothing on page 1 or 3 can be acted on. Page 2 is left alone: the
        // join in flight ends on its own, within its watchdog at worst, and
        // reports how.
        if (!m_rows.isEmpty()) {
            publishRows(QList<WifiRow>(), QString());
        }
        // Whatever the rows hold now is no scan of the state the box comes
        // back to. The return scan applyStatus() starts can be refused by a
        // busy slot, and the connection can come back unchanged, so the
        // mismatch test alone would never ask again.
        m_rowsStale = true;
        // Page 3 always goes: that is the safety rule, since its actions
        // must never run against a link nobody can vouch for. Page 1 goes
        // only when the adapter or the radio has gone, not on state 4: one
        // status read that answered slowly (the GUI can be busy, and so can
        // NetworkManager) would otherwise throw away a half-typed password.
        // Keeping it is safe, because submitJoin() refuses outside states 0
        // and 1, so nothing typed there can act until a read succeeds.
        const int currentPage = page();
        if (currentPage == kPageManage ||
                (currentPage == kPagePassword && state != kStateNotResponding)) {
            goToList();
        }
        return;
    }
    if (connectionChanged && page() == kPageManage) {
        // The network page 3 was managing is no longer the one the box is on,
        // and its Disconnect would act on whatever the box is on now.
        goToList();
    }
    if (!m_visibleClients.isEmpty()) {
        // While someone is looking, rescan on every change of state or
        // network, so the active flag in the list catches up with the link.
        startScan(false);
    }
}

void WifiSettings::updateStatusLine(bool stateChanged) {
    QString statusLine;
    switch (m_state) {
    case kStateConnected:
        statusLine = tr("Connected to %1").arg(connectedSsid());
        if (!m_connectedIpv4.isEmpty()) {
            // U+00B7 MIDDLE DOT, which DejaVu Sans renders, spelled as a code
            // point so the source stays ASCII.
            statusLine += QStringLiteral(" %1 ").arg(QChar(0x00B7)) + m_connectedIpv4;
        }
        break;
    case kStateNoAdapter:
        statusLine = tr("No Wi-Fi adapter");
        break;
    case kStateRadioOff:
        statusLine = tr("Wi-Fi is off");
        break;
    case kStateNotResponding:
        statusLine = tr("Network service not responding");
        break;
    default:
        statusLine = tr("Not connected");
        break;
    }
    if (!stateChanged && statusLine == m_statusLine) {
        return;
    }
    m_statusLine = statusLine;
    emit statusChanged(m_statusLine, m_state);
}

QString WifiSettings::connectedSsid() const {
    if (m_rowsConnection == m_connectedProfile) {
        for (const WifiRow& row : m_rows) {
            if (row.active) {
                return row.ssid;
            }
        }
    }
    return connectedProfileSsid();
}

QString WifiSettings::connectedProfileSsid() const {
    if (!m_connectedUuid.isEmpty()) {
        for (const SavedProfile& profile : m_savedProfiles) {
            if (profile.uuid == m_connectedUuid) {
                return profile.ssid;
            }
        }
    }
    return ssidOfProfile(m_connectedProfile);
}

int WifiSettings::savedProfileNameCount(const QString& name) const {
    return static_cast<int>(std::count_if(m_savedProfiles.cbegin(),
            m_savedProfiles.cend(),
            [&name](const SavedProfile& profile) { return profile.name == name; }));
}

QString WifiSettings::ssidOfProfile(const QString& name) const {
    for (const SavedProfile& profile : m_savedProfiles) {
        if (profile.name == name) {
            return profile.ssid;
        }
    }
    return name;
}

const WifiSettings::SavedProfile* WifiSettings::savedProfileForSsid(const QString& ssid) const {
    for (const SavedProfile& profile : m_savedProfiles) {
        if (profile.ssid == ssid) {
            return &profile;
        }
    }
    return nullptr;
}

QString WifiSettings::uniqueUuidOfProfile(const QString& name) const {
    QString uuid;
    for (const SavedProfile& profile : m_savedProfiles) {
        if (profile.name != name) {
            continue;
        }
        if (!uuid.isEmpty()) {
            return QString();
        }
        uuid = profile.uuid;
    }
    return uuid;
}

QStringList WifiSettings::savedSsids() const {
    QStringList ssids;
    for (const SavedProfile& profile : m_savedProfiles) {
        ssids.append(profile.ssid);
    }
    return ssids;
}

bool WifiSettings::isActiveNow(const WifiRow& row) const {
    if (m_state != kStateConnected) {
        return false;
    }
    // Closes the window between a link change and the rescan landing, in
    // which the current network's row would otherwise not count as active
    // and a tap on it would start joining the network the box is already
    // on. By SSID, through the saved profiles, never by the profile's name:
    // on a box connected by a profile called "Cafe" that joins "Home", the
    // row "Cafe" is some other network, and page 3 for it would disconnect
    // Home.
    if (!m_connectedProfile.isEmpty() && row.ssid == connectedProfileSsid()) {
        return true;
    }
    return row.active && m_rowsConnection == m_connectedProfile;
}

void WifiSettings::rescanIfRowsStale() {
    if (!m_visibleClients.isEmpty() &&
            (m_rowsStale || m_rowsConnection != m_connectedProfile)) {
        startScan(false);
    }
}

bool WifiSettings::manageTargetStillConnected() {
    // Captured first: a link change seen by the refresh sends page 3 back to
    // the list, which clears the target.
    const QString target = m_joinTarget;
    refreshStatus();
    // By SSID only. Page 3 names a network, and a bare comparison with the
    // profile's name would let a profile called "Cafe" that joins "Home"
    // pass for the network "Cafe".
    if (page() == kPageManage && m_state == kStateConnected && !target.isEmpty() &&
            target == connectedSsid()) {
        return true;
    }
    goToList();
    if (m_state == kStateNotResponding) {
        // Nothing is known about the link, least of all that it has gone.
        // Saying so would send the DJ looking for a fault that is not there.
        qInfo() << "WifiSettings: page 3 action dropped, NetworkManager is not responding";
        notify(tr("Network service not responding, try again"),
                Notifications::Severity::Warning);
        return false;
    }
    qInfo() << "WifiSettings: page 3 action dropped," << target << "is no longer connected";
    if (!target.isEmpty()) {
        notify(tr("No longer connected to %1").arg(target), Notifications::Severity::Info);
    }
    return false;
}

bool WifiSettings::startScan(bool explicitRescan) {
    // A scan is never the op that pre-empts: a busy slot simply refuses it.
    // So does a join between claiming the slot and starting its nmcli, a
    // window in which a page change can show a widget whose
    // setClientVisible() asks for a scan.
    if (!isUsableState()) {
        return false;
    }
    if (m_opInFlight || m_joinInFlight) {
        // Remembered, so rescanIfRowsStale() asks again once the slot frees
        // (or on the next 10 s tick). A scan already in flight clears this
        // again when it lands, which answers the refused request too.
        m_rowsStale = true;
        return false;
    }
    const QString rescan = explicitRescan ? QStringLiteral("yes") : QStringLiteral("auto");
    const QString connectionAtStart = m_connectedProfile;
    // Set before runNmcli(), which can call back before it returns. Cleared
    // only by onScanFinished(), where every path of every step ends.
    m_scanInFlight = true;
    // Saved profiles first, so the list is flagged against a view no older
    // than the scan itself. Their SSIDs, not their names, are what a row is
    // matched against, and this listing cannot show an SSID, so it only
    // says which profiles are Wi-Fi ones and what their UUIDs are.
    runNmcli({"-t", "-f", "NAME,UUID,TYPE", "connection", "show"},
            kScanWatchdogMs,
            [this, rescan, connectionAtStart](const NmcliResult& listed) {
                if (!listed.succeeded()) {
                    onScanFinished(listed, connectionAtStart);
                    return;
                }
                const QList<SavedProfile> wifiProfiles = parseWifiProfileList(listed.out);
                if (wifiProfiles.isEmpty()) {
                    // Nothing saved: no SSIDs to read, and `connection show`
                    // with no uuid after it would list every profile again.
                    m_savedProfiles.clear();
                    startScanList(rescan, connectionAtStart);
                    return;
                }
                // One call for every Wi-Fi profile, each as its own "uuid"
                // and value pair of argv elements.
                QStringList args{"-t",
                        "-f",
                        "connection.id,connection.uuid,802-11-wireless.ssid",
                        "connection",
                        "show"};
                for (const SavedProfile& profile : wifiProfiles) {
                    args << QStringLiteral("uuid") << profile.uuid;
                }
                // Cannot be refused: callbacks run with the op slot free.
                const bool started = runNmcli(args,
                        kScanWatchdogMs,
                        [this, rescan, connectionAtStart](const NmcliResult& details) {
                            if (!details.succeeded()) {
                                onScanFinished(details, connectionAtStart);
                                return;
                            }
                            m_savedProfiles = parseSavedWifiProfiles(details.out);
                            startScanList(rescan, connectionAtStart);
                        });
                if (!started) {
                    m_scanInFlight = false;
                    m_pCoScan->set(0.0);
                }
            });
    return true;
}

void WifiSettings::startScanList(const QString& rescan, const QString& connectionAtStart) {
    // Cannot be refused: called from a callback, with the op slot free.
    const bool started = runNmcli(
            {"-t", "-f", "IN-USE,SSID,SIGNAL,SECURITY", "device", "wifi", "list", "--rescan", rescan},
            kScanWatchdogMs,
            [this, connectionAtStart](const NmcliResult& list) {
                onScanFinished(list, connectionAtStart);
            });
    if (!started) {
        m_scanInFlight = false;
        m_pCoScan->set(0.0);
    }
}

void WifiSettings::onScanFinished(const NmcliResult& result, const QString& connectionAtStart) {
    m_scanInFlight = false;
    m_pCoScan->set(0.0);
    if (result.cancelled) {
        // Pre-empted by a user action (Ruling 13). What it found is thrown
        // away, and nothing is started from here: the action is waiting for
        // the slot. Marked stale so the action's end (rescanIfRowsStale())
        // or the next refresh restores it, even with the connection
        // unchanged.
        qInfo() << "WifiSettings: scan pre-empted, result discarded";
        m_rowsStale = true;
        return;
    }
    if (!result.succeeded()) {
        // Not the "Wi-Fi scan:" prefix, on purpose: check-log.sh filters that
        // one as healthy, and a scan that failed is a fault it should show.
        // Once per run of failures, though, not once per 10 s tick: the page
        // left open with NetworkManager wedged would otherwise fill the log
        // someone is reading to work out what is wrong with the box. A
        // success, or a failure for a different reason, is a new line.
        const QString reason = describeFailure(result);
        if (reason != m_lastScanFailure) {
            qWarning().noquote() << "Wi-Fi scan failed:" << reason;
            m_lastScanFailure = reason;
        }
        return;
    }
    m_lastScanFailure.clear();
    QList<WifiRow> rows = parseWifiList(result.out, savedSsids());
    // Warning level so it reaches the log at all: the appliance runs at the
    // default warning threshold, which drops info lines. Exactly one per
    // completed scan, and check-log.sh filters it by this prefix.
    qWarning().noquote() << QStringLiteral("Wi-Fi scan: %1 %2")
                                    .arg(rows.size())
                                    .arg(rows.size() == 1 ? QStringLiteral("network")
                                                          : QStringLiteral("networks"));
    if (!isUsableState()) {
        // The radio went off, or NetworkManager went away, mid-scan.
        return;
    }
    publishRows(std::move(rows), connectionAtStart);
    if (connectionAtStart != m_connectedProfile && !m_visibleClients.isEmpty()) {
        // The link changed while this scan ran, so its active flags may
        // describe the old connection and are not trusted (isActiveNow()).
        // The rescan applyStatus() asked for was refused while this one held
        // the slot; run it now that the slot is free.
        startScan(false);
    }
}

void WifiSettings::publishRows(QList<WifiRow> rows, const QString& connection) {
    m_rows = std::move(rows);
    m_rowsConnection = connection;
    // The clearing call in applyStatus() marks the rows stale again itself.
    m_rowsStale = false;
    m_pCoNetworkCount->forceSet(static_cast<double>(m_rows.size()));
    emit networksChanged(m_rows);
    // The status line names the active row's SSID when the rows allow it.
    updateStatusLine(false);
}

// static
QString WifiSettings::joinProfileName(const JoinTarget& target, JoinCommand command) {
    return command == JoinCommand::ConnectionUp ? target.profileName : target.ssid;
}

bool WifiSettings::startJoin(JoinTarget target, JoinCommand command) {
    if (!claimOpSlot()) {
        return false;
    }
    const QString ssid = target.ssid;
    // Holds the slot from here, through the page change and the snapshot
    // read, until runNmcli() takes it: startScan() refuses while this is set.
    m_joinInFlight = true;

    // Page 2 before the snapshot read below, which can take about 3 s when
    // NetworkManager is wedged (its 2 s bound plus the reap), so the page is already the joining page by
    // the time the GUI next paints.
    setJoinTarget(ssid);
    setPage(kPageJoining);

    // Before anything is created: every profile name as it stands, so a
    // failure can tell a profile this attempt made from one the DJ already
    // had. Taken for saved networks too, where it simply names the target and
    // so protects it.
    // The same read also says which profile the box is on right now, fresher
    // than the last status read, which can be 10 s old.
    QString profiles;
    QString error;
    if (readNmcliSync({"-t", "-f", "NAME,TYPE,DEVICE", "connection", "show"},
                kSyncReadTimeoutMs,
                &profiles,
                &error)) {
        m_joinSnapshot = allProfileNames(profiles);
        m_joinStartProfile = parseActiveProfile(profiles, m_wifiDevice);
    } else {
        // Both unknown now. Not m_connectedProfile as a stand-in: it can be
        // stale, and a stale guess would let a cancel take down the live link.
        // The snapshot reset is defensive: every way a join ends (success,
        // failure, cancel) already consumes or clears it, so none should be
        // left here. The start-profile reset is not: nothing else clears
        // that one, so without it the previous join's would stand in.
        m_joinSnapshot.reset();
        m_joinStartProfile.reset();
        qWarning() << "WifiSettings: could not list profiles before joining" << ssid
                   << "so a failed or cancelled attempt will not be cleaned up:" << error;
    }

    // The SSID, password and device are each one argv element, whatever
    // characters they hold.
    QStringList args;
    if (command == JoinCommand::ConnectionUp) {
        // By UUID: the name need not be unique, and the SSID need not be the
        // name at all. The name is only a fallback for a profile whose UUID
        // the scan did not report, which parseSavedWifiProfiles() does not
        // let through; kept so the command is never "up uuid" and nothing.
        if (!target.profileUuid.isEmpty()) {
            args = QStringList{"--wait",
                    kConnectWaitSeconds,
                    "connection",
                    "up",
                    "uuid",
                    target.profileUuid};
        } else {
            args = QStringList{
                    "--wait", kConnectWaitSeconds, "connection", "up", "id", target.profileName};
        }
    } else {
        args = QStringList{"--wait", kConnectWaitSeconds, "device", "wifi", "connect", ssid};
        if (command == JoinCommand::ConnectWithPassword) {
            args << QStringLiteral("password") << m_password;
        }
        args << QStringLiteral("ifname") << m_wifiDevice;
    }
    // Never log args: it can hold the password.
    qInfo() << "WifiSettings: joining" << ssid
            << (command == JoinCommand::ConnectionUp ? "by its saved profile" : "as a new network");

    // Every "started" change is made by now, before runNmcli(), which can call
    // back before it returns.
    if (!runNmcli(args, kConnectWatchdogMs, [this, target, command](const NmcliResult& result) {
            onJoinFinished(target, command, result);
        })) {
        // Unreachable: the slot was claimed above, and m_joinInFlight has kept
        // scans out of it since.
        m_joinInFlight = false;
        m_joinSnapshot.reset();
        goToList();
        return false;
    }
    return true;
}

void WifiSettings::onJoinFinished(
        const JoinTarget& target, JoinCommand command, const NmcliResult& result) {
    m_joinInFlight = false;
    m_pCoJoin->set(0.0);
    const QString& ssid = target.ssid;
    // What the snapshot and the start profile, both lists of profile names,
    // are compared against: never the SSID for a saved profile, whose name
    // can be anything.
    const QString profileName = joinProfileName(target, command);

    // On every failure path below, the follow-up nmcli op starts before any
    // page change. A page change can show a Wi-Fi widget, and its
    // setClientVisible() would otherwise start a scan into the slot this op
    // needs.
    if (result.cancelled) {
        qInfo() << "WifiSettings: join of" << ssid << "cancelled";
        // Killing nmcli does not stop NetworkManager, which carries on with
        // the activation it was asked for and can connect after the DJ
        // cancelled. Deleting a profile this attempt created stops it as
        // well. Where there is nothing to delete (a saved profile brought up
        // by `connection up`, a password tried on a profile that already
        // existed) take the connection down, unless it is, or may be, the
        // one the box was on when the join began: that one would come back
        // by itself, but `connection down` blocks its autoconnect and
        // strands a box whose only link is this Wi-Fi. When the start
        // profile is unknown (the snapshot read failed), nothing is taken
        // down: a cancelled join that goes on to complete is recoverable.
        if (!cleanupResidue(profileName)) {
            if (!m_joinStartProfile) {
                qInfo() << "WifiSettings: not taking" << profileName
                        << "down after the cancel, the profile the box was on is unknown";
            } else if (profileName == *m_joinStartProfile) {
                // By name, not UUID. With two profiles of one name this
                // can only err towards leaving a link up, never towards
                // taking the one the box was on down.
                qInfo() << "WifiSettings: not taking" << profileName
                        << "down after the cancel, the box was on it when the join began";
            } else {
                stopActivation(profileName,
                        command == JoinCommand::ConnectionUp ? target.profileUuid : QString());
                // `connection down` also blocks that network's autoconnect
                // until something asks for it explicitly, so the DJ who
                // cancels while the box is on nothing is left with no link
                // and no sign that one more tap is all it takes.
                notify(tr("Cancelled. Tap the network again to connect."),
                        Notifications::Severity::Info);
            }
        }
        goToList();
        refreshStatus();
        rescanIfRowsStale();
        return;
    }
    if (result.succeeded()) {
        m_joinSnapshot.reset();
        goToList();
        refreshStatus();
        startScan(false);
        notify(tr("Connected to %1").arg(ssid), Notifications::Severity::Info);
        return;
    }

    // Before the buffer is cleared: the redaction in describeFailure() needs it.
    const QString detail = describeFailure(result);
    cleanupResidue(profileName);
    if (result.started && !result.timedOut && isWrongPasswordError(result.err)) {
        // Info: a mistyped password is the DJ's business, not a fault.
        qInfo() << "WifiSettings: join of" << ssid << "rejected the password";
        clearPassword();
        setJoinTarget(ssid);
        setPage(kPagePassword);
        notify(tr("Wrong password for %1").arg(ssid), Notifications::Severity::Warning);
    } else {
        qWarning() << "WifiSettings: joining" << ssid << "failed:" << detail;
        goToList();
        notify(detail, Notifications::Severity::Error);
    }
    // Starting a join tears down whatever the device was on, so the state
    // shown before the attempt is stale either way.
    refreshStatus();
    // Refused if the residue delete holds the slot; its callback catches up.
    rescanIfRowsStale();
}

bool WifiSettings::cleanupResidue(const QString& profileName) {
    const std::optional<QStringList> snapshot = std::exchange(m_joinSnapshot, std::nullopt);
    // No snapshot: nothing proves a profile by that name is new, so leave it.
    // Named in the snapshot: it was there before the attempt and is the DJ's.
    // A saved profile brought up by `connection up` always is, by its own
    // name, which is why the name and not the SSID is what is looked for.
    if (!snapshot || profileName.isEmpty() || snapshot->contains(profileName)) {
        return false;
    }
    // By name: a profile `device wifi connect` has only just created has no
    // UUID this class ever saw.
    const bool started = runNmcli(
            {"--wait", kShortWaitSeconds, "connection", "delete", "id", profileName},
            kDisconnectWatchdogMs,
            [this, profileName](const NmcliResult& result) {
                // "unknown connection" is the ordinary outcome when the attempt
                // never got as far as creating a profile.
                if (result.succeeded()) {
                    qInfo() << "WifiSettings: removed the profile" << profileName
                            << "a failed join left behind";
                } else {
                    qInfo() << "WifiSettings: no profile" << profileName
                            << "to remove after a failed join" << result.err.simplified();
                }
                // The rescan the failed join's link change asked for was
                // refused while this delete held the slot.
                rescanIfRowsStale();
            });
    if (!started) {
        qWarning() << "WifiSettings: another nmcli call is running, so the profile"
                   << profileName << "a failed join created may remain";
    }
    return started;
}

void WifiSettings::stopActivation(const QString& profileName, const QString& profileUuid) {
    // Called from onJoinFinished() with the slot just freed, so this cannot be
    // refused in practice. "not an active connection" is the ordinary outcome
    // when NetworkManager had already given up on its own.
    const QStringList args = profileUuid.isEmpty()
            ? QStringList{"--wait", kShortWaitSeconds, "connection", "down", "id", profileName}
            : QStringList{"--wait", kShortWaitSeconds, "connection", "down", "uuid", profileUuid};
    const bool started = runNmcli(args,
            kDisconnectWatchdogMs,
            [this, profileName](const NmcliResult& result) {
                qInfo() << "WifiSettings: took" << profileName << "down after a cancelled join:"
                        << (result.succeeded() ? QStringLiteral("done")
                                               : result.err.simplified());
                refreshStatus();
                rescanIfRowsStale();
            });
    if (!started) {
        qWarning() << "WifiSettings: another nmcli call is running, so the cancelled join of"
                   << profileName << "may still connect";
    }
}

bool WifiSettings::startDisconnect() {
    if (!isUsableState() || page() != kPageManage || m_wifiDevice.isEmpty()) {
        return false;
    }
    // `device disconnect` acts on whatever the device is on, so check that is
    // still the network page 3 names before touching it.
    if (!manageTargetStillConnected() || !claimOpSlot()) {
        return false;
    }
    const QString ssid = m_joinTarget;
    return runNmcli({"--wait", kShortWaitSeconds, "device", "disconnect", m_wifiDevice},
            kDisconnectWatchdogMs,
            [this, ssid](const NmcliResult& result) {
                m_pCoDisconnect->set(0.0);
                goToList();
                refreshStatus();
                if (result.succeeded()) {
                    startScan(false);
                    // `device disconnect` also stops NetworkManager
                    // autoconnecting the device. Without saying so, the DJ
                    // waits for a reconnect that never comes and reads the
                    // button as broken.
                    notify(tr("Disconnected from %1. It will not reconnect on its "
                              "own until you join it again.")
                                    .arg(ssid),
                            Notifications::Severity::Info);
                    return;
                }
                const QString detail = describeFailure(result);
                qWarning() << "WifiSettings: disconnect failed:" << detail;
                notify(detail, Notifications::Severity::Error);
                rescanIfRowsStale();
            });
}

bool WifiSettings::startForget() {
    if (!isUsableState() || page() != kPageManage || m_joinTarget.isEmpty()) {
        return false;
    }
    if (!manageTargetStillConnected()) {
        return false;
    }
    // The profile to delete is the one the box is on, the one the status read
    // just validated. Page 3 can name a network by its SSID where the profile
    // is called something else, and `connection delete id <ssid>` would then
    // fail, or hit an unrelated profile named like the SSID. By the active
    // connection's UUID, from the status read just taken, so a second profile
    // sharing the name is never the one deleted. `connection delete id` with
    // a shared name could delete both, so when that UUID is unknown the name
    // is only used where no saved profile, or exactly one, has it.
    const QString profile = m_connectedProfile;
    const QString ssid = m_joinTarget;
    if (profile.isEmpty()) {
        return false;
    }
    QString uuid = m_connectedUuid;
    if (uuid.isEmpty()) {
        if (savedProfileNameCount(profile) > 1) {
            qWarning() << "WifiSettings: not forgetting" << profile
                       << "- more than one saved profile has that name and the status read"
                       << "could not say which one is active";
            goToList();
            notify(tr("Could not tell which saved profile for %1 to forget, try again")
                            .arg(ssid),
                    Notifications::Severity::Warning);
            return false;
        }
        uuid = uniqueUuidOfProfile(profile);
    }
    // Before the delete, while m_savedProfiles still lists the profile.
    const QString forgottenSsid = connectedProfileSsid();
    if (!claimOpSlot()) {
        return false;
    }
    const QStringList args = uuid.isEmpty()
            ? QStringList{"--wait", kShortWaitSeconds, "connection", "delete", "id", profile}
            : QStringList{"--wait", kShortWaitSeconds, "connection", "delete", "uuid", uuid};
    return runNmcli(args,
            kDisconnectWatchdogMs,
            [this, profile, uuid, ssid, forgottenSsid](const NmcliResult& result) {
                m_pCoForget->set(0.0);
                // Counted before anything below can start the scan that
                // replaces m_savedProfiles: the other profiles that join the
                // same network, which this deliberately did not delete.
                const bool otherProfileRemains = std::any_of(m_savedProfiles.cbegin(),
                        m_savedProfiles.cend(),
                        [&](const SavedProfile& saved) {
                            return saved.ssid == forgottenSsid &&
                                    (uuid.isEmpty() ? saved.name != profile
                                                    : saved.uuid != uuid);
                        });
                goToList();
                refreshStatus();
                if (result.succeeded()) {
                    startScan(false);
                    if (otherProfileRemains) {
                        // "Forgot Home" would be untrue while another saved
                        // profile still joins Home, so name the one that went.
                        notify(tr("Forgot the saved profile %1. %2 is still saved.")
                                        .arg(profile, ssid),
                                Notifications::Severity::Info);
                    } else {
                        // Named as page 3 named it, by the SSID the DJ tapped.
                        notify(tr("Forgot %1").arg(ssid), Notifications::Severity::Info);
                    }
                    return;
                }
                const QString detail = describeFailure(result);
                qWarning() << "WifiSettings: forgetting" << ssid << "(profile" << profile
                           << ") failed:" << detail;
                notify(detail, Notifications::Severity::Error);
                rescanIfRowsStale();
            });
}

bool WifiSettings::startRadioOn() {
    if (m_state != kStateRadioOff) {
        return false;
    }
    if (!claimOpSlot()) {
        return false;
    }
    return runNmcli({"radio", "wifi", "on"},
            kRadioWatchdogMs,
            [this](const NmcliResult& result) {
                m_pCoRadioOn->set(0.0);
                refreshStatus();
                if (result.succeeded()) {
                    startScan(false);
                    // The refresh above may have read the radio as still off.
                    // One more look once it has settled, bound to this so a
                    // teardown drops it; applyStatus() takes it from there if
                    // the state did change.
                    QTimer::singleShot(kRadioSettleMs, this, &WifiSettings::refreshStatus);
                    return;
                }
                const QString detail = describeFailure(result);
                qWarning() << "WifiSettings: turning the radio on failed:" << detail;
                notify(detail, Notifications::Severity::Error);
                rescanIfRowsStale();
            });
}

int WifiSettings::page() const {
    const double value = m_pCoPage->get();
    // The CO is writable from outside (the control socket, and the skin's
    // WidgetStack), so it may hold anything, and anything that is not one of
    // the four pages reads as -1, no page. Not 0: the list is a real page
    // whose actions this would then allow.
    //
    // NaN needs util_isnan() and cannot be caught by a comparison here. Mixxx
    // is built with -ffast-math (CMakeLists.txt:226), under which the compiler
    // may assume no operand is NaN and fold a guard like !(value >= kPageList)
    // away; what is left is static_cast<int>(NaN), which is 0 on aarch64 --
    // the list page, on the one box this ships to. util_isnan() is the
    // deliberately un-inlined wrapper compiled without that flag, and this is
    // the same reason ControllerScriptInterfaceLegacy screens the values
    // scripts write to COs (controllerscriptinterfacelegacy.cpp:196).
    if (util_isnan(value) || value < kPageList || value > kPageManage) {
        return -1;
    }
    return static_cast<int>(value);
}

void WifiSettings::setPage(int page) {
    m_pCoPage->set(static_cast<double>(page));
}

void WifiSettings::setJoinTarget(const QString& ssid) {
    m_joinTarget = ssid;
    emit joinTargetChanged(m_joinTarget);
}

void WifiSettings::clearPassword() {
    m_password.clear();
    // Unconditional, like joinTargetChanged: a keypad entering page 1 resets
    // its display on it whether or not the buffer held anything.
    emit passwordChanged(0);
}

void WifiSettings::goToList() {
    clearPassword();
    setJoinTarget(QString());
    setPage(kPageList);
}

int WifiSettings::selectedIndex() const {
    const double value = m_pCoSelectedIndex->get();
    // -1, no row, for NaN, for a negative index and for one past the end of
    // the list as it stands now. An in-range fraction truncates to the row it
    // names. See page() for why NaN takes util_isnan() rather than a
    // comparison.
    if (util_isnan(value) || value < 0.0 || value >= static_cast<double>(m_rows.size())) {
        return -1;
    }
    return static_cast<int>(value);
}

QString WifiSettings::describeFailure(const NmcliResult& result) const {
    QString text;
    if (result.cancelled) {
        text = tr("Cancelled");
    } else if (result.timedOut) {
        text = tr("NetworkManager did not answer in time");
    } else if (!result.started) {
        text = tr("Could not run nmcli: %1").arg(result.err);
    } else {
        text = result.err;
        // Masked before simplifying, which could otherwise split a password
        // with runs of spaces in it so it no longer matches.
        if (!m_password.isEmpty()) {
            text.replace(m_password, QStringLiteral("********"));
        }
        text = text.simplified();
        if (text.isEmpty()) {
            text = result.normalExit
                    ? tr("nmcli exited with code %1").arg(result.exitCode)
                    : tr("nmcli was stopped before it finished");
        }
    }
    return text;
}

void WifiSettings::notifyBusy() {
    notify(tr("Wi-Fi is busy, try again in a moment"), Notifications::Severity::Warning);
}
