#include "engine/sidechain/visualsserver.h"

#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>

#include "engine/sidechain/visualsfeed.h"
#include "moc_visualsserver.cpp"
#include "util/assert.h"

namespace {

// The request is "GET /events HTTP/1.1" plus a handful of headers.
// Chromium's EventSource GET carries User-Agent, sec-ch-ua*, Accept*,
// Referer and Sec-Fetch-* headers, realistically 600-900 bytes, so the cap
// has to clear that with room to spare; 4096 still bounds what a client that
// never sends the blank line can grow, and this listener is loopback only.
constexpr qsizetype kMaxRequestBytes = 4096;
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
    // VisualsFeed is not this object's child (the sidechain owns it, see its
    // own header), so its lifetime is not tied to ours. If it goes first,
    // null the pointer rather than leave it dangling for /status to read.
    connect(m_pFeed, &QObject::destroyed, this, [this]() { m_pFeed = nullptr; });

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
        const bool enabled = m_pFeed && m_pFeed->enabled();
        const QByteArray body = QByteArray("{\"enabled\":") +
                (enabled ? "1" : "0") +
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
            // Stalled reader: it is past this threshold precisely because it
            // never drains its buffer, so disconnectFromHost() would wait on
            // that same buffer with no timeout and reclaim nothing. abort()
            // discards the unsent bytes, closes the socket immediately and
            // emits disconnected() synchronously, which is why the erase
            // happens first: onDisconnected() runs inside this call and must
            // not find the socket still in m_streams.
            it = m_streams.erase(it);
            pSocket->abort();
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
