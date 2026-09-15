#include "preferences/wifisettings.h"

#include <QProcess>
#include <QProcessEnvironment>
#include <QtDebug>
#include <algorithm>
#include <utility>

#include "control/controlobject.h"
#include "control/controlpushbutton.h"
#include "moc_wifisettings.cpp"
#include "notifications/notifications.h"

namespace {
const QString kGroup = QStringLiteral("[Wifi]");
const QString kNmcli = QStringLiteral("nmcli");

// Status refresh period while a Wi-Fi widget is on screen. Often enough that a
// network joined or lost behind the DJ's back shows up while they are looking
// at the page, and never at all while they are not.
constexpr int kStatusRefreshMs = 10000;

// Bound on each synchronous status read. nmcli answers a local D-Bus query in
// tens of milliseconds, so this only fires when NetworkManager is wedged, and
// it is how long the GUI thread stalls when it is. Deliberately not the 30 s
// default of QProcess::waitForFinished(), which the tryUnmount() pattern this
// is modelled on still uses.
constexpr int kSyncReadTimeoutMs = 2000;

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

// How nmcli reports a rejected passphrase. "Secrets were required" is
// NetworkManager's own activation failure; newer nmcli words the same failure
// "Passwords or encryption keys are required"; and a passphrase NM refuses
// outright is reported against the psk property.
bool isWrongPasswordError(const QString& error) {
    return error.contains(QLatin1String("Secrets were required"), Qt::CaseInsensitive) ||
            error.contains(QLatin1String("Passwords or encryption keys are required"),
                    Qt::CaseInsensitive) ||
            error.contains(QLatin1String("psk"), Qt::CaseInsensitive);
}

// One synchronous nmcli read for the status refresh: the tryUnmount() shape,
// with an explicit bound instead of waitForFinished()'s 30 s default. Only a
// normal exit 0 counts as an answer, and then stdout goes to *pOut; anything
// else writes a reason to *pError. The process is reaped before returning
// either way. Never pumps the event loop, so it cannot re-enter this class.
bool readNmcliSync(const QStringList& args, QString* pOut, QString* pError) {
    QProcess process;
    process.setProcessEnvironment(nmcliEnvironment());
    process.start(kNmcli, args);
    if (!process.waitForFinished(kSyncReadTimeoutMs)) {
        QString error;
        if (process.error() == QProcess::FailedToStart) {
            error = QStringLiteral("could not run nmcli: ") + process.errorString();
        } else {
            error = QStringLiteral("nmcli did not answer within %1 ms").arg(kSyncReadTimeoutMs);
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
    connect(&m_statusTimer, &QTimer::timeout, this, &WifiSettings::refreshStatus);

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

    if (row.active) {
        setJoinTarget(row.ssid);
        setPage(kPageManage);
        return;
    }
    if (row.enterprise) {
        notify(tr("Enterprise Wi-Fi is not supported"), Notifications::Severity::Warning);
        return;
    }
    if (row.saved) {
        startJoin(row.ssid, JoinCommand::ConnectionUp);
        return;
    }
    if (!row.secured) {
        startJoin(row.ssid, JoinCommand::Connect);
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
    startJoin(m_joinTarget, JoinCommand::ConnectWithPassword);
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
        const QString& output, const QStringList& savedWifiNames) {
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
        row.saved = savedWifiNames.contains(row.ssid);
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
QStringList WifiSettings::parseSavedWifiNames(const QString& output) {
    QStringList names;
    const QStringList lines = outputLines(output);
    for (const QString& line : lines) {
        const QStringList fields = splitTerseFields(line);
        if (fields.size() < 2 || fields.at(0).isEmpty()) {
            continue;
        }
        if (fields.at(1) == QLatin1String("802-11-wireless")) {
            names.append(fields.at(0));
        }
    }
    names.removeDuplicates();
    return names;
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

void WifiSettings::onScanRequested(double value) {
    if (value == 0.0) {
        return;
    }
    // A dead end in state 2 and 4, like every action. State 3 still lets Scan
    // re-check, since the radio may have been switched on from elsewhere.
    if (m_state != kStateNoAdapter && m_state != kStateNotResponding) {
        refreshStatus();
        if (startScan(true)) {
            // onScanFinished() puts the CO back when the scan ends.
            return;
        }
    }
    m_pCoScan->set(0.0);
}

void WifiSettings::onJoinRequested(double value) {
    if (value == 0.0) {
        return;
    }
    if (isUsableState()) {
        const int currentPage = page();
        if (currentPage == kPageList) {
            activateRow(selectedIndex());
        } else if (currentPage == kPagePassword) {
            submitJoin();
        }
    }
    // A join that started owns the CO until it ends (onJoinFinished() resets
    // it). Every other outcome of the tap has already ended here.
    if (!m_joinInFlight) {
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
    if (!m_pOpProcess) {
        return;
    }
    // Info, not warning: every caller reports its own failure, and one fault
    // should be one line in the log.
    qInfo() << "WifiSettings: nmcli outlived its watchdog, killing it";
    NmcliResult result;
    result.started = true;
    result.timedOut = true;
    completeOp(m_pOpProcess, result);
}

bool WifiSettings::runNmcli(const QStringList& args, int watchdogMs, NmcliCallback callback) {
    if (m_opInFlight) {
        return false;
    }
    m_opInFlight = true;
    m_opCallback = std::move(callback);

    // Owned, never startDetached, for the reason SystemSettings::
    // runPowerCommand gives: a polkit refusal is a non-zero exit after a
    // perfectly good exec, which only an owned process can see. Here success
    // also has to hand stdout to a parser.
    auto* pProcess = new QProcess(this);
    m_pOpProcess = pProcess;
    pProcess->setProcessEnvironment(nmcliEnvironment());

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
    m_opInFlight = false;
    // Moved out before the call: the callback may start the next op, which
    // installs a callback of its own.
    NmcliCallback callback = std::move(m_opCallback);
    m_opCallback = nullptr;
    if (callback) {
        callback(result);
    }
}

void WifiSettings::abortOp() {
    if (!m_pOpProcess) {
        return;
    }
    NmcliResult result;
    result.started = true;
    result.cancelled = true;
    completeOp(m_pOpProcess, result);
}

void WifiSettings::refreshStatus() {
    QString output;
    QString error;
    const auto reportNotResponding = [this, &error]() {
        if (m_state != kStateNotResponding) {
            qWarning() << "WifiSettings: NetworkManager is not responding:" << error;
        }
        applyStatus(kStateNotResponding, tr("Network service not responding"));
    };

    if (!readNmcliSync({"-t", "-f", "DEVICE,TYPE,STATE,CONNECTION", "device", "status"},
                &output,
                &error)) {
        reportNotResponding();
        return;
    }
    const std::optional<WifiDevice> device = parseWifiDevice(output);
    if (!device) {
        m_wifiDevice.clear();
        applyStatus(kStateNoAdapter, tr("No Wi-Fi adapter"));
        return;
    }
    m_wifiDevice = device->device;

    // The software radio switch, the one `nmcli radio wifi on` flips. The
    // exact form measured on bitepi in Phase 0.
    if (!readNmcliSync({"-t", "-f", "WIFI", "general"}, &output, &error)) {
        reportNotResponding();
        return;
    }
    if (output.trimmed() != QLatin1String("enabled")) {
        applyStatus(kStateRadioOff, tr("Wi-Fi is off"));
        return;
    }

    // Covers "connected" and "connected (externally)". Everything else,
    // including the "connecting (...)" steps of a join in progress, is not
    // connected yet.
    if (!device->state.startsWith(QLatin1String("connected"))) {
        applyStatus(kStateDisconnected, tr("Not connected"));
        return;
    }
    // CONNECTION is the profile name, which is the SSID for every profile
    // `device wifi connect` creates, and fresh from this very read, where the
    // scanned rows may be up to a scan old.
    QString statusLine = tr("Connected to %1").arg(device->connection);
    // The last read, so a failure here has nothing left to short-circuit:
    // report the link without an address rather than call a working link dead.
    if (readNmcliSync({"-t", "-f", "IP4.ADDRESS", "device", "show", m_wifiDevice},
                &output,
                &error)) {
        const QString ipv4 = parseIpv4(output);
        if (!ipv4.isEmpty()) {
            // U+00B7 MIDDLE DOT, which DejaVu Sans renders, spelled as a code
            // point so the source stays ASCII.
            statusLine += QStringLiteral(" %1 ").arg(QChar(0x00B7)) + ipv4;
        }
    }
    applyStatus(kStateConnected, statusLine);
}

void WifiSettings::applyStatus(int state, const QString& statusLine) {
    const int oldState = m_state;
    const bool wasUsable = isUsableState();
    const bool lineChanged = statusLine != m_statusLine;
    m_state = state;
    m_statusLine = statusLine;
    if (state != oldState) {
        m_pCoState->forceSet(static_cast<double>(state));
    }
    if (state != oldState || lineChanged) {
        emit statusChanged(m_statusLine, m_state);
    }
    if (state == oldState) {
        return;
    }

    if (!isUsableState()) {
        // No adapter, radio off or no answer: the list means nothing, and
        // nothing on page 1 or 3 can be acted on. Page 2 is left alone: the
        // join in flight ends on its own, within its watchdog at worst, and
        // reports how.
        if (!m_rows.isEmpty()) {
            publishRows(QList<WifiRow>());
        }
        const int currentPage = page();
        if (currentPage == kPagePassword || currentPage == kPageManage) {
            goToList();
        }
        return;
    }
    if (oldState == kStateConnected && page() == kPageManage) {
        // The network page 3 was managing is no longer the one the box is on.
        goToList();
    }
    if (!wasUsable && !m_visibleClients.isEmpty()) {
        // Back from no adapter, radio off or no answer while someone is
        // looking: fill the list this state change has made meaningful again.
        startScan(false);
    }
}

bool WifiSettings::startScan(bool explicitRescan) {
    if (!isUsableState()) {
        return false;
    }
    const QString rescan = explicitRescan ? QStringLiteral("yes") : QStringLiteral("auto");
    // Saved profile names first, so the list is flagged against a view no
    // older than the scan itself.
    return runNmcli({"-t", "-f", "NAME,TYPE", "connection", "show"},
            kScanWatchdogMs,
            [this, rescan](const NmcliResult& profiles) {
                if (!profiles.succeeded()) {
                    onScanFinished(profiles);
                    return;
                }
                m_savedWifiNames = parseSavedWifiNames(profiles.out);
                // Cannot be refused: callbacks run with the op slot free.
                const bool started = runNmcli(
                        {"-t", "-f", "IN-USE,SSID,SIGNAL,SECURITY",
                                "device", "wifi", "list", "--rescan", rescan},
                        kScanWatchdogMs,
                        [this](const NmcliResult& list) {
                            onScanFinished(list);
                        });
                if (!started) {
                    m_pCoScan->set(0.0);
                }
            });
}

void WifiSettings::onScanFinished(const NmcliResult& result) {
    m_pCoScan->set(0.0);
    if (!result.succeeded()) {
        // Not the "Wi-Fi scan:" prefix, on purpose: check-log.sh filters that
        // one as healthy, and a scan that failed is a fault it should show.
        qWarning().noquote() << "Wi-Fi scan failed:" << describeFailure(result);
        return;
    }
    QList<WifiRow> rows = parseWifiList(result.out, m_savedWifiNames);
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
    publishRows(std::move(rows));
}

void WifiSettings::publishRows(QList<WifiRow> rows) {
    m_rows = std::move(rows);
    m_pCoNetworkCount->forceSet(static_cast<double>(m_rows.size()));
    emit networksChanged(m_rows);
}

bool WifiSettings::startJoin(QString ssid, JoinCommand command) {
    if (m_opInFlight) {
        notifyBusy();
        return false;
    }

    // Before anything is created: every profile name as it stands, so a
    // failure can tell a profile this attempt made from one the DJ already
    // had. Taken for saved networks too, where it simply names the target and
    // so protects it.
    QString profiles;
    QString error;
    if (readNmcliSync({"-t", "-f", "NAME,TYPE", "connection", "show"}, &profiles, &error)) {
        m_joinSnapshot = allProfileNames(profiles);
    } else {
        m_joinSnapshot.reset();
        qWarning() << "WifiSettings: could not list profiles before joining" << ssid
                   << "so a failed attempt will not be cleaned up:" << error;
    }

    // The SSID, password and device are each one argv element, whatever
    // characters they hold.
    QStringList args;
    if (command == JoinCommand::ConnectionUp) {
        args = QStringList{"--wait", kConnectWaitSeconds, "connection", "up", "id", ssid};
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

    // Every "started" change before runNmcli(), which can call back before it
    // returns.
    setJoinTarget(ssid);
    setPage(kPageJoining);
    m_joinInFlight = true;
    if (!runNmcli(args, kConnectWatchdogMs, [this, ssid](const NmcliResult& result) {
            onJoinFinished(ssid, result);
        })) {
        // Unreachable: the slot was free above and nothing has run since.
        m_joinInFlight = false;
        m_joinSnapshot.reset();
        goToList();
        return false;
    }
    return true;
}

void WifiSettings::onJoinFinished(const QString& ssid, const NmcliResult& result) {
    m_joinInFlight = false;
    m_pCoJoin->set(0.0);

    if (result.cancelled) {
        qInfo() << "WifiSettings: join of" << ssid << "cancelled";
        goToList();
        cleanupResidue(ssid);
        refreshStatus();
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
    cleanupResidue(ssid);
    // Starting a join tears down whatever the device was on, so the state
    // shown before the attempt is stale either way.
    refreshStatus();
}

void WifiSettings::cleanupResidue(const QString& ssid) {
    const std::optional<QStringList> snapshot = std::exchange(m_joinSnapshot, std::nullopt);
    // No snapshot: nothing proves a profile by that name is new, so leave it.
    // Named in the snapshot: it was there before the attempt and is the DJ's.
    if (!snapshot || snapshot->contains(ssid)) {
        return;
    }
    const bool started = runNmcli({"--wait", kShortWaitSeconds, "connection", "delete", "id", ssid},
            kDisconnectWatchdogMs,
            [ssid](const NmcliResult& result) {
                // "unknown connection" is the ordinary outcome when the attempt
                // never got as far as creating a profile.
                if (result.succeeded()) {
                    qInfo() << "WifiSettings: removed the profile a failed join of" << ssid
                            << "left behind";
                } else {
                    qInfo() << "WifiSettings: no profile to remove after the failed join of"
                            << ssid << result.err.simplified();
                }
            });
    if (!started) {
        qWarning() << "WifiSettings: another nmcli call is running, so a profile the failed"
                   << "join of" << ssid << "created may remain";
    }
}

bool WifiSettings::startDisconnect() {
    if (!isUsableState() || page() != kPageManage || m_wifiDevice.isEmpty()) {
        return false;
    }
    if (m_opInFlight) {
        notifyBusy();
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
            });
}

bool WifiSettings::startForget() {
    if (!isUsableState() || page() != kPageManage || m_joinTarget.isEmpty()) {
        return false;
    }
    if (m_opInFlight) {
        notifyBusy();
        return false;
    }
    const QString ssid = m_joinTarget;
    return runNmcli({"--wait", kShortWaitSeconds, "connection", "delete", "id", ssid},
            kDisconnectWatchdogMs,
            [this, ssid](const NmcliResult& result) {
                m_pCoForget->set(0.0);
                goToList();
                refreshStatus();
                if (result.succeeded()) {
                    startScan(false);
                    notify(tr("Forgot %1").arg(ssid), Notifications::Severity::Info);
                    return;
                }
                const QString detail = describeFailure(result);
                qWarning() << "WifiSettings: forgetting" << ssid << "failed:" << detail;
                notify(detail, Notifications::Severity::Error);
            });
}

bool WifiSettings::startRadioOn() {
    if (m_state != kStateRadioOff) {
        return false;
    }
    if (m_opInFlight) {
        notifyBusy();
        return false;
    }
    return runNmcli({"radio", "wifi", "on"},
            kRadioWatchdogMs,
            [this](const NmcliResult& result) {
                m_pCoRadioOn->set(0.0);
                refreshStatus();
                if (result.succeeded()) {
                    startScan(false);
                    return;
                }
                const QString detail = describeFailure(result);
                qWarning() << "WifiSettings: turning the radio on failed:" << detail;
                notify(detail, Notifications::Severity::Error);
            });
}

int WifiSettings::page() const {
    const double value = m_pCoPage->get();
    // Also rejects NaN. The CO is writable from outside (the control socket),
    // so it may hold anything.
    if (!(value >= kPageList) || value > kPageManage) {
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
    if (!(value >= 0.0) || value >= static_cast<double>(m_rows.size())) {
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
