#include "engine/sidechain/visualsserver.h"

#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>

#include "engine/sidechain/visualsfeed.h"
#include "moc_visualsserver.cpp"
#include "util/assert.h"

namespace {

// The request is "GET /events HTTP/1.1" plus a handful of headers; 512 is
// generous and bounds what a client that never sends the blank line can grow.
constexpr qsizetype kMaxRequestBytes = 512;
// A client that stops reading must be dropped before its unsent frames grow
// the process. 64 KB is about 300 frames, ten seconds of stall.
constexpr qint64 kMaxUnsentBytes = 64 * 1024;

const QByteArray kHeaderEnd("\r\n\r\n");

} // namespace

VisualsServer::VisualsServer(VisualsFeed* pFeed, quint16 port, QObject* pParent)
        : QObject(pParent),
          m_pFeed(pFeed),
          m_pServer(new QTcpServer(this)) {
    DEBUG_ASSERT(m_pFeed);
    connect(m_pServer, &QTcpServer::newConnection, this, &VisualsServer::onNewConnection);
    connect(m_pFeed, &VisualsFeed::frameReady, this, &VisualsServer::broadcastFrame);

    if (!m_pServer->listen(QHostAddress::LocalHost, port)) {
        qWarning().noquote() << "visuals feed: cannot listen on"
                              << QStringLiteral("127.0.0.1:%1").arg(port)
                              << m_pServer->errorString();
        return;
    }
    // Warning level on purpose, for the same reason as the control socket's
    // line: the appliance logs at Warning and an info line would leave no
    // evidence the server came up. Filtered in toolchain/skin/check-log.sh.
    qWarning().noquote() << "visuals feed: listening on"
                          << QStringLiteral("127.0.0.1:%1").arg(m_pServer->serverPort());
}

VisualsServer::~VisualsServer() = default;

quint16 VisualsServer::serverPort() const {
    return m_pServer->serverPort();
}

int VisualsServer::clientCount() const {
    return static_cast<int>(m_streams.size());
}

void VisualsServer::onNewConnection() {
    while (QTcpSocket* pSocket = m_pServer->nextPendingConnection()) {
        connect(pSocket, &QTcpSocket::readyRead, this, &VisualsServer::onReadyRead);
        connect(pSocket, &QTcpSocket::disconnected, this, &VisualsServer::onDisconnected);
        m_pending.insert(pSocket, QByteArray());
    }
}

void VisualsServer::onReadyRead() {
    QTcpSocket* pSocket = qobject_cast<QTcpSocket*>(sender());
    VERIFY_OR_DEBUG_ASSERT(pSocket) {
        return;
    }
    if (m_streams.contains(pSocket)) {
        // A streaming client has nothing more to say; discard it.
        pSocket->readAll();
        return;
    }
    const auto it = m_pending.find(pSocket);
    if (it == m_pending.end()) {
        pSocket->readAll();
        return;
    }
    it->append(pSocket->readAll());
    if (it->size() > kMaxRequestBytes) {
        pSocket->write(
                "HTTP/1.1 431 Request Header Fields Too Large\r\n"
                "Connection: close\r\nContent-Length: 0\r\n\r\n");
        pSocket->disconnectFromHost();
        m_pending.remove(pSocket);
        return;
    }
    const qsizetype end = it->indexOf(kHeaderEnd);
    if (end < 0) {
        return;
    }
    // kHeaderEnd itself begins with "\r\n", so once it is found there is
    // always an earlier (or equal) "\r\n" ending the request line. The guard
    // is defensive: a malformed buffer where that lookup somehow fails must
    // not turn into QByteArray::left(-1), which returns the whole buffer.
    const qsizetype lineEnd = it->indexOf("\r\n");
    const QByteArray requestLine = lineEnd >= 0 ? it->left(lineEnd) : QByteArray();
    m_pending.remove(pSocket);
    handleRequest(pSocket, requestLine);
}

void VisualsServer::handleRequest(QTcpSocket* pSocket, const QByteArray& requestLine) {
    const QList<QByteArray> parts = requestLine.split(' ');
    const QByteArray method = parts.value(0);
    const QByteArray path = parts.value(1);

    if (method == "GET" && path == "/events") {
        pSocket->write(
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "Cache-Control: no-cache\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Connection: keep-alive\r\n\r\n");
        m_streams.insert(pSocket);
        return;
    }

    if (method == "GET" && path == "/status") {
        const QByteArray body = QByteArray("{\"enabled\":") +
                (m_pFeed->enabled() ? "1" : "0") +
                ",\"clients\":" + QByteArray::number(clientCount()) + "}";
        pSocket->write(
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Connection: close\r\n"
                "Content-Length: " +
                QByteArray::number(body.size()) + "\r\n\r\n" + body);
        pSocket->disconnectFromHost();
        return;
    }

    pSocket->write("HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
    pSocket->disconnectFromHost();
}

void VisualsServer::broadcastFrame(const QByteArray& json) {
    const QByteArray event = "data: " + json + "\n\n";
    for (auto it = m_streams.begin(); it != m_streams.end();) {
        QTcpSocket* pSocket = *it;
        if (pSocket->bytesToWrite() > kMaxUnsentBytes) {
            // Stalled reader. Dropping it is what keeps a wedged Chromium
            // from growing this process for the rest of the set.
            it = m_streams.erase(it);
            pSocket->disconnectFromHost();
            continue;
        }
        pSocket->write(event);
        ++it;
    }
}

void VisualsServer::onDisconnected() {
    QTcpSocket* pSocket = qobject_cast<QTcpSocket*>(sender());
    VERIFY_OR_DEBUG_ASSERT(pSocket) {
        return;
    }
    m_pending.remove(pSocket);
    m_streams.remove(pSocket);
    pSocket->deleteLater();
}
