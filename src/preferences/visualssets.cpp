#include "preferences/visualssets.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkInterface>
#include <QtDebug>
#include <cmath>
#include <utility>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "moc_visualssets.cpp"

namespace {

const QString kGroup = QStringLiteral("[BiteDJ]");
const QString kActiveItem = QStringLiteral("visuals_set");
const QString kRevItem = QStringLiteral("visuals_set_rev");
const QString kPageItem = QStringLiteral("visuals_page");
const QString kPinItem = QStringLiteral("visuals_admin_pin");
const ConfigKey kSettingsTabKey(QStringLiteral("[SettingsTab]"), QStringLiteral("current"));

constexpr int kDebounceMs = 200;
constexpr int kPollMs = 10000;
constexpr int kIpRefreshMs = 10000;

// A set's knob name in the file, the control it is written to, and the
// values the Visuals page's segment rows can show. The file is written by a
// program on the LAN, so nothing outside these reaches a control.
struct KnobSpec {
    const char* name;
    const char* control;
    int min;
    int max;
};
const KnobSpec kKnobSpecs[] = {
        {"bars", "visuals_bars", 8, 64},
        {"reactivity", "visuals_reactivity", 0, 2},
        {"bounce", "visuals_bounce", 0, 3},
        {"swirl", "visuals_swirl", 0, 3},
        {"camMix", "visuals_cam_mix", 0, 1},
};

bool knobValueAllowed(const KnobSpec& spec, int value) {
    if (value < spec.min || value > spec.max) {
        return false;
    }
    if (qstrcmp(spec.name, "bars") == 0) {
        return value == 8 || value == 16 || value == 32 || value == 64;
    }
    return true;
}

// A JSON number that is a whole number, as an int. Anything else is false.
bool wholeNumber(const QJsonValue& value, int* pOut) {
    if (!value.isDouble()) {
        return false;
    }
    const double d = value.toDouble();
    if (!std::isfinite(d) || std::floor(d) != d || d < -1e9 || d > 1e9) {
        return false;
    }
    *pOut = static_cast<int>(d);
    return true;
}

} // namespace

QAtomicPointer<VisualsSets> VisualsSets::s_pInstance = nullptr;

VisualsSets::VisualsSets(UserSettingsPointer pConfig, const QString& setsPath, QObject* parent)
        : QObject(parent),
          m_pConfig(std::move(pConfig)),
          m_setsPath(setsPath),
          m_fileState(FileState::Missing),
          m_haveRead(false),
          m_lastExisted(false),
          m_haveKnobBaseline(false),
          m_knobBaselineId(0) {
    const ConfigKey activeKey(kGroup, kActiveItem);
    m_pCoActive = std::make_unique<ControlObject>(activeKey);
    m_pCoActive->set(m_pConfig->getValue(activeKey, 0));
    connect(m_pCoActive.get(),
            &ControlObject::valueChanged,
            this,
            &VisualsSets::onActiveChanged);

    // Seeded before setReadOnly(), which would refuse the write. Epoch
    // seconds fit a double exactly and stay under 2^31 until 2038.
    m_pCoRev = std::make_unique<ControlObject>(ConfigKey(kGroup, kRevItem));
    m_pCoRev->set(static_cast<double>(QDateTime::currentSecsSinceEpoch()));
    m_pCoRev->setReadOnly();

    // Pre-created, as [Wifi],page is, so the skin's WidgetStack binds to this
    // object and has a value to read on its first showEvent.
    m_pCoPage = std::make_unique<ControlObject>(ConfigKey(kGroup, kPageItem));
    connect(m_pCoPage.get(),
            &ControlObject::valueChanged,
            this,
            &VisualsSets::onPageChanged);

    m_pPinProxy = std::make_unique<ControlProxy>(kGroup,
            kPinItem,
            this,
            ControlFlag::AllowMissingOrInvalid | ControlFlag::NoWarnIfMissing);
    m_pPinProxy->connectValueChanged(this, &VisualsSets::onPinChanged);
    bindSettingsTab(false);

    m_debounce.setSingleShot(true);
    m_debounce.setInterval(kDebounceMs);
    connect(&m_debounce, &QTimer::timeout, this, &VisualsSets::reload);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString&) {
        m_debounce.start();
    });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString&) {
        m_debounce.start();
    });
    m_poll.setSingleShot(false);
    m_poll.setInterval(kPollMs);
    connect(&m_poll, &QTimer::timeout, this, [this]() {
        reload();
        bindSettingsTab(false);
    });
    m_poll.start();

    m_ipTimer.setSingleShot(false);
    m_ipTimer.setInterval(kIpRefreshMs);
    connect(&m_ipTimer, &QTimer::timeout, this, &VisualsSets::refreshAdminInfo);

    s_pInstance.storeRelease(this);
    reload();
    refreshAdminInfo();
}

VisualsSets::~VisualsSets() {
    s_pInstance.testAndSetRelease(this, nullptr);
    m_poll.stop();
    m_debounce.stop();
    m_ipTimer.stop();
    for (const QMetaObject::Connection& connection : std::as_const(m_visibleClients)) {
        QObject::disconnect(connection);
    }
    m_visibleClients.clear();
}

// static
QString VisualsSets::defaultSetsPath() {
    return QDir::homePath() + QStringLiteral("/.bitedj-visuals-sets.json");
}

// static
QString VisualsSets::formatPin(double value) {
    int pin = kDefaultPin;
    if (std::isfinite(value) && std::floor(value) == value && value >= 0 && value <= 9999) {
        pin = static_cast<int>(value);
    }
    return QStringLiteral("%1").arg(pin, 4, 10, QLatin1Char('0'));
}

// static
QString VisualsSets::currentIpv4() {
    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface& iface : interfaces) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning) ||
                (flags & QNetworkInterface::IsLoopBack)) {
            continue;
        }
        const QList<QNetworkAddressEntry> addresses = iface.addressEntries();
        for (const QNetworkAddressEntry& entry : addresses) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                return entry.ip().toString();
            }
        }
    }
    return QString();
}

QList<VisualsSets::Entry> VisualsSets::entries() const {
    QList<Entry> list;
    list.append(Entry{0, tr("Everything")});
    for (const Set& set : m_sets) {
        list.append(Entry{set.id, set.name});
    }
    return list;
}

int VisualsSets::activeId() const {
    return static_cast<int>(m_pCoActive->get());
}

int VisualsSets::activeRow() const {
    const int id = activeId();
    for (int i = 0; i < m_sets.size(); ++i) {
        if (m_sets.at(i).id == id) {
            return i + 1;
        }
    }
    return 0;
}

QString VisualsSets::activeName() const {
    if (m_fileState == FileState::Unreadable) {
        return tr("Everything (sets file unreadable)");
    }
    const Set* pSet = findSet(activeId());
    return pSet ? pSet->name : tr("Everything");
}

const VisualsSets::Set* VisualsSets::findSet(int id) const {
    for (const Set& set : m_sets) {
        if (set.id == id) {
            return &set;
        }
    }
    return nullptr;
}

void VisualsSets::selectRow(int row, int returnPage) {
    const QList<Entry> list = entries();
    if (row < 0 || row >= list.size()) {
        return;
    }
    const int id = list.at(row).id;
    if (id == activeId()) {
        // visuals_set ignores a write of the value it holds (control.cpp:294),
        // so onActiveChanged would never run. Picking the active set again
        // is how the panel gets its knobs back, so apply them here.
        if (const Set* pSet = findSet(id)) {
            applyKnobs(pSet->knobs);
            rememberKnobs(id, pSet->knobs);
        }
        qInfo() << "VisualsSets: active set" << id << "picked again, knobs reapplied";
    } else {
        // Static set: a null sender, so onActiveChanged runs through the
        // same path as a tap on the panel or a co.sh write.
        ControlObject::set(ConfigKey(kGroup, kActiveItem), id);
    }
    ControlObject::set(ConfigKey(kGroup, kPageItem), returnPage);
}

void VisualsSets::onPageChanged(double value) {
    // Someone just opened a sub-page: the moment the tab watch has to be
    // live. A write of 0 is skipped because onSettingsTabChanged makes it
    // from inside the tab proxy's own slot, where rebuilding that proxy
    // would delete it under its caller.
    if (value != 0.0) {
        bindSettingsTab(true);
    }
}

void VisualsSets::onSettingsTabChanged(double /*value*/) {
    if (m_pCoPage->get() != 0.0) {
        // Static set, so the skin's WidgetStack hears it.
        ControlObject::set(ConfigKey(kGroup, kPageItem), 0.0);
    }
}

void VisualsSets::bindSettingsTab(bool force) {
    if (m_pSettingsTabProxy && m_pSettingsTabProxy->valid() && !force) {
        return;
    }
    if (!ControlObject::exists(kSettingsTabKey)) {
        return;
    }
    m_pSettingsTabProxy = std::make_unique<ControlProxy>(kSettingsTabKey, this);
    m_pSettingsTabProxy->connectValueChanged(this, &VisualsSets::onSettingsTabChanged);
}

void VisualsSets::setClientVisible(QObject* client, bool visible) {
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
        refreshAdminInfo();
        m_ipTimer.start();
    } else if (wasAnyVisible && !isAnyVisible) {
        m_ipTimer.stop();
    }
}

// static
bool VisualsSets::parse(const QByteArray& bytes, QList<Set>* pSets, QString* pError) {
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        *pError = parseError.errorString();
        return false;
    }
    if (!doc.isObject()) {
        *pError = QStringLiteral("not a JSON object");
        return false;
    }
    const QJsonObject root = doc.object();
    int version = 0;
    if (!wholeNumber(root.value(QStringLiteral("version")), &version) || version != 1) {
        *pError = QStringLiteral("version is not 1");
        return false;
    }
    const QJsonValue setsValue = root.value(QStringLiteral("sets"));
    if (!setsValue.isArray()) {
        *pError = QStringLiteral("sets is not an array");
        return false;
    }
    QList<Set> sets;
    const QJsonArray array = setsValue.toArray();
    for (const QJsonValue& value : array) {
        const QJsonObject object = value.toObject();
        int id = 0;
        if (!wholeNumber(object.value(QStringLiteral("id")), &id) || id <= 0) {
            qWarning() << "VisualsSets: skipping a set with no usable id";
            continue;
        }
        bool duplicate = false;
        for (const Set& seen : std::as_const(sets)) {
            duplicate = duplicate || seen.id == id;
        }
        if (duplicate) {
            qWarning() << "VisualsSets: skipping a second set with id" << id;
            continue;
        }
        Set set;
        set.id = id;
        set.name = object.value(QStringLiteral("name")).toString().trimmed();
        if (set.name.isEmpty()) {
            set.name = tr("Set %1").arg(id);
        }
        const QJsonObject knobs = object.value(QStringLiteral("knobs")).toObject();
        for (const KnobSpec& spec : kKnobSpecs) {
            const QString name = QString::fromLatin1(spec.name);
            if (!knobs.contains(name)) {
                continue;
            }
            int knobValue = 0;
            if (wholeNumber(knobs.value(name), &knobValue) && knobValueAllowed(spec, knobValue)) {
                set.knobs.insert(name, knobValue);
            } else {
                qWarning() << "VisualsSets: set" << id << "knob" << name
                           << "has a value the panel cannot show; left as it is";
            }
        }
        sets.append(set);
    }
    *pSets = sets;
    return true;
}

void VisualsSets::reload() {
    QFile file(m_setsPath);
    const bool exists = file.exists();
    QByteArray bytes;
    bool readOk = false;
    if (exists && file.open(QIODevice::ReadOnly)) {
        bytes = file.readAll();
        readOk = true;
    }
    rearmWatches();
    if (m_haveRead && exists == m_lastExisted && bytes == m_lastBytes) {
        return;
    }
    const bool firstRead = !m_haveRead;
    m_haveRead = true;
    m_lastExisted = exists;
    m_lastBytes = bytes;

    QList<Set> sets;
    QString error;
    if (!exists) {
        m_fileState = FileState::Missing;
    } else if (!readOk) {
        m_fileState = FileState::Unreadable;
        qWarning() << "VisualsSets:" << m_setsPath << "unreadable:" << file.errorString();
    } else if (!parse(bytes, &sets, &error)) {
        m_fileState = FileState::Unreadable;
        qWarning() << "VisualsSets:" << m_setsPath << "unreadable:" << error;
    } else {
        m_fileState = FileState::Valid;
    }
    m_sets = m_fileState == FileState::Valid ? sets : QList<Set>();
    if (!firstRead) {
        m_pCoRev->forceSet(m_pCoRev->get() + 1.0);
    }

    const int id = activeId();
    if (m_fileState != FileState::Unreadable && id != 0) {
        const Set* pSet = findSet(id);
        if (!pSet) {
            fallBackToEverything(id);
            return;
        }
        if (m_haveKnobBaseline && m_knobBaselineId == id && pSet->knobs != m_knobBaseline) {
            qInfo() << "VisualsSets: set" << id << "knobs edited, applying them";
            applyKnobs(pSet->knobs);
        }
        rememberKnobs(id, pSet->knobs);
    }
    emitState();
}

void VisualsSets::onActiveChanged(double value) {
    const int id = static_cast<int>(value);
    m_pConfig->setValue(ConfigKey(kGroup, kActiveItem), id);
    if (id != 0 && m_fileState != FileState::Unreadable) {
        const Set* pSet = findSet(id);
        if (!pSet) {
            fallBackToEverything(id);
            return;
        }
        applyKnobs(pSet->knobs);
        rememberKnobs(id, pSet->knobs);
    } else if (id == 0) {
        rememberKnobs(0, Knobs());
    }
    qInfo() << "VisualsSets: active set" << id << activeName();
    emitState();
}

void VisualsSets::fallBackToEverything(int missingId) {
    qInfo() << "VisualsSets: set" << missingId << "is not in the sets file, back to Everything";
    // Static set: onActiveChanged(0) persists it and emits the state.
    ControlObject::set(ConfigKey(kGroup, kActiveItem), 0.0);
}

void VisualsSets::rememberKnobs(int id, const Knobs& knobs) {
    m_haveKnobBaseline = true;
    m_knobBaselineId = id;
    m_knobBaseline = knobs;
}

void VisualsSets::applyKnobs(const Knobs& knobs) {
    for (const KnobSpec& spec : kKnobSpecs) {
        const QString name = QString::fromLatin1(spec.name);
        if (!knobs.contains(name)) {
            continue;
        }
        const ConfigKey key(kGroup, QString::fromLatin1(spec.control));
        if (!ControlObject::exists(key)) {
            continue;
        }
        // Static set, for the reason in the header comment.
        ControlObject::set(key, knobs.value(name));
    }
}

void VisualsSets::rearmWatches() {
    const QString dir = QFileInfo(m_setsPath).absolutePath();
    if (!m_watcher.directories().contains(dir) && QDir(dir).exists()) {
        m_watcher.addPath(dir);
    }
    if (!m_watcher.files().contains(m_setsPath) && QFile::exists(m_setsPath)) {
        m_watcher.addPath(m_setsPath);
    }
}

void VisualsSets::onPinChanged(double /*value*/) {
    refreshAdminInfo();
}

void VisualsSets::refreshAdminInfo() {
    m_ipv4 = currentIpv4();
    const double pin = m_pPinProxy->valid() ? m_pPinProxy->get() : kDefaultPin;
    const QString text = QStringLiteral("%1.local:%2 %3 %4 %3 PIN %5")
                                 .arg(QHostInfo::localHostName(),
                                         QString::number(kAdminPort),
                                         QString(QChar(0x00B7)),
                                         m_ipv4.isEmpty() ? tr("no network") : m_ipv4,
                                         formatPin(pin));
    if (text == m_adminInfo) {
        return;
    }
    m_adminInfo = text;
    emit adminInfoChanged(m_adminInfo);
}

void VisualsSets::emitState() {
    emit entriesChanged(entries(), activeRow());
    emit activeNameChanged(activeName());
}
