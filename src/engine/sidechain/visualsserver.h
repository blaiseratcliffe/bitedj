#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QtGlobal>

class QTcpServer;
class QTcpSocket;
class VisualsFeed;

/// Bite DJ: pushes VisualsFeed frames to the visuals page.
///
/// Server-Sent Events over a hand-rolled HTTP/1.1 subset, because the Pi's
/// cross-compile sysroot has Qt6Network but not Qt WebSockets, and SSE is a
/// response that never ends: headers once, then "data: <json>\n\n" per frame.
/// Chromium's EventSource reconnects on its own after an app restart, so the
/// page needs no logic for that.
///
///   GET /events   text/event-stream, one frame per VisualsFeed tick
///   GET /status   {"enabled":0|1,"clients":n}, then the connection closes
///   anything else 404
///
/// Loopback only and read-only: nothing a client sends changes a control, so
/// unlike ControlSocket the listener is always up. pi/bin/bitedj-visuals polls
/// /status to learn whether to launch or kill Chromium; the frames only flow
/// while [BiteDJ],visuals_enabled is on.
class VisualsServer : public QObject {
    Q_OBJECT
  public:
    static constexpr quint16 kDefaultPort = 7374;

    // port 0 asks the OS for an ephemeral port (tests).
    VisualsServer(VisualsFeed* pFeed,
            quint16 port = kDefaultPort,
            QObject* pParent = nullptr);
    ~VisualsServer() override;

    quint16 serverPort() const;
    int clientCount() const;

  public slots:
    void broadcastFrame(const QByteArray& json);

  private slots:
    void onNewConnection();
    void onReadyRead();
    void onDisconnected();

  private:
    void handleRequest(QTcpSocket* pSocket, const QByteArray& requestLine);

    VisualsFeed* m_pFeed;
    QTcpServer* m_pServer;
    /// Request bytes per client until the header block's blank line arrives.
    QHash<QTcpSocket*, QByteArray> m_pending;
    /// Clients that asked for /events and now receive every frame.
    QSet<QTcpSocket*> m_streams;
};
