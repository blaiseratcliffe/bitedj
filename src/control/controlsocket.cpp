#include "control/controlsocket.h"

#include "moc_controlsocket.cpp"

#include <QHostAddress>
#include <QLatin1String>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtGlobal>

#include "control/controlobject.h"
#include "util/assert.h"

namespace {

constexpr char kEnvVar[] = "BITEDJ_CONTROL";

// Cap on how much unterminated input one client may buffer. A request is a
// verb, a key and a number; 512 is far more than that and small enough that a
// client which never sends a newline cannot grow the process.
constexpr qsizetype kMaxLineBytes = 512;

const QString kBye = QStringLiteral("bye");

/// Format a control value for the wire.
///
/// 'g' with 12 significant digits is chosen so the values this appliance
/// actually stores come back as themselves: waveform_visual_gain is 0.75 and
/// must not print as 0.74999999999999989, waveform_type is 26 and must not
/// print as 26.000000. Both are what a shorter or fixed format does here.
QString formatValue(double value) {
    return QString::number(value, 'g', 12);
}

} // namespace

// static
std::optional<std::uint16_t> ControlSocket::configuredPort() {
    const QByteArray raw = qgetenv(kEnvVar);
    if (raw.isEmpty()) {
        return std::nullopt;
    }
    bool ok = false;
    const uint port = QString::fromLatin1(raw).trimmed().toUInt(&ok);
    if (!ok || port == 0 || port > 65535) {
        // A fault, not noise: someone asked for the socket and will otherwise
        // sit waiting for a port that was never opened. Deliberately not
        // filtered in toolchain/skin/check-log.sh.
        qWarning() << "control socket:" << kEnvVar
                   << "is not a usable port, ignoring:" << raw;
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(port);
}

ControlSocket::ControlSocket(std::uint16_t port, QObject* parent)
        : QObject(parent),
          m_pServer(new QTcpServer(this)) {
    connect(m_pServer,
            &QTcpServer::newConnection,
            this,
            &ControlSocket::onNewConnection);

    // The address is built into one string rather than streamed, because
    // QDebug puts a space before every argument and "127.0.0.1: 7373" is not
    // an address anyone can paste.
    const QString endpoint = QStringLiteral("127.0.0.1:%1").arg(port);

    // LocalHost, never Any. The appliance sits on a LAN and this reaches every
    // control in the process, so it must not be a network service.
    if (!m_pServer->listen(QHostAddress::LocalHost, port)) {
        qWarning().noquote() << "control socket: cannot listen on" << endpoint
                             << m_pServer->errorString();
        return;
    }

    // Warning level on purpose, the same reasoning as the "3Band source:" and
    // "QuickEffect preset order:" lines: kLogLevelDefault is Warning and the
    // appliance launches without --logLevel, so an info line would never reach
    // the log and there would be no evidence the socket came up. This healthy
    // line IS filtered in check-log.sh; the two failures above are not.
    qWarning().noquote() << "control socket: listening on" << endpoint;
}

ControlSocket::~ControlSocket() = default;

void ControlSocket::onNewConnection() {
    while (QTcpSocket* pSocket = m_pServer->nextPendingConnection()) {
        connect(pSocket,
                &QTcpSocket::readyRead,
                this,
                &ControlSocket::onReadyRead);
        connect(pSocket,
                &QTcpSocket::disconnected,
                this,
                &ControlSocket::onDisconnected);
        m_buffers.insert(pSocket, QByteArray());
    }
}

void ControlSocket::onReadyRead() {
    QTcpSocket* pSocket = qobject_cast<QTcpSocket*>(sender());
    VERIFY_OR_DEBUG_ASSERT(pSocket) {
        return;
    }
    const auto it = m_buffers.find(pSocket);
    VERIFY_OR_DEBUG_ASSERT(it != m_buffers.end()) {
        return;
    }

    it->append(pSocket->readAll());

    // Checked before anything is sliced, not after. A client that sends a
    // newline only past the cap must be cut off rather than acted on, and
    // indexOf returns qsizetype, so a truncating int here would pick the wrong
    // slice out of an oversized buffer instead of rejecting it.
    if (it->size() > kMaxLineBytes) {
        pSocket->write("err line too long\n");
        pSocket->disconnectFromHost();
        return;
    }

    qsizetype newline = it->indexOf('\n');
    while (newline >= 0) {
        const QByteArray raw = it->left(newline);
        it->remove(0, newline + 1);
        const QString reply = handleLine(QString::fromUtf8(raw).trimmed());
        if (!reply.isEmpty()) {
            pSocket->write(reply.toUtf8());
            pSocket->write("\n");
        }
        if (reply == kBye) {
            pSocket->disconnectFromHost();
            return;
        }
        newline = it->indexOf('\n');
    }
}

void ControlSocket::onDisconnected() {
    QTcpSocket* pSocket = qobject_cast<QTcpSocket*>(sender());
    VERIFY_OR_DEBUG_ASSERT(pSocket) {
        return;
    }
    m_buffers.remove(pSocket);
    pSocket->deleteLater();
}

QString ControlSocket::handleLine(const QString& line) const {
    const QStringList parts = line.split(QChar(' '), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        // A blank line gets no reply, so a stray newline cannot desynchronise
        // a client that reads one line per request.
        return QString();
    }

    const QString verb = parts.at(0).toLower();
    if (verb == QLatin1String("quit")) {
        return kBye;
    }

    if (parts.size() < 2) {
        return QStringLiteral("err usage: get|set|exists <group>,<key> [value]");
    }

    const ConfigKey key = ConfigKey::parseCommaSeparated(parts.at(1));

    if (verb == QLatin1String("exists")) {
        return ControlObject::exists(key) ? QStringLiteral("1")
                                          : QStringLiteral("0");
    }

    if (verb == QLatin1String("get")) {
        // Answering 0 for a key that does not exist would be indistinguishable
        // from a control that is genuinely 0, which is exactly the confusion
        // this socket exists to remove. ControlObject::get cannot tell them
        // apart, so ask first.
        if (!ControlObject::exists(key)) {
            return QStringLiteral("err no such control");
        }
        return formatValue(ControlObject::get(key));
    }

    if (verb == QLatin1String("set")) {
        if (parts.size() < 3) {
            return QStringLiteral("err set needs a value");
        }
        bool ok = false;
        const double value = parts.at(2).toDouble(&ok);
        if (!ok) {
            return QStringLiteral("err value is not a number");
        }
        if (!ControlObject::exists(key)) {
            return QStringLiteral("err no such control");
        }
        ControlObject::set(key, value);
        // The readback, not the requested value. See the class comment: this is
        // the only place a swallowed write becomes visible.
        return formatValue(ControlObject::get(key));
    }

    return QStringLiteral("err unknown command");
}
