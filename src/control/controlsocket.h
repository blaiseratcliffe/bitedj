#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <cstdint>
#include <optional>

#include "preferences/configobject.h"

class QTcpServer;
class QTcpSocket;

/// Bite DJ: a line-based control surface over ControlObject, for driving and
/// inspecting the appliance from a script.
///
/// It exists because verification had no readback. The panel can be tapped
/// without a finger (toolchain/skin/tap.sh) and captured without one
/// (toolchain/skin/shoot.sh), but nothing could answer "did that setting
/// actually take". A lit segment in a screenshot is evidence the skin binding
/// resolves, not evidence the control moved, and a control the skin writes
/// while something else silently overrides it looks identical either way. That
/// gap is not hypothetical here: docs/M4-SKIN-NOTES.md section 24.2 is a row
/// that shipped, was recorded as verified, and was being reset on every launch.
///
/// OFF unless the BITEDJ_CONTROL environment variable names a TCP port, and
/// bound to 127.0.0.1 only. Treat it as a full control surface when it is on:
/// every ControlObject in the process is reachable, which includes transport,
/// the library and the [System] power actions. It is meant for a development
/// session, not for a gig.
///
/// Protocol: one request a line, one reply a line, UTF-8.
///
///   get <group>,<key>            -> the value, or "err no such control"
///   set <group>,<key> <value>    -> the value READ BACK afterwards
///   exists <group>,<key>         -> "1" or "0"
///   quit                         -> "bye", then the connection closes
///
/// `set` answering with the readback rather than an "ok" is the whole point. A
/// read-only control accepts the write and discards it (ControlObject::set
/// takes the same change-request path a skin widget does, and
/// ControlObject::setReadOnly installs a handler that only warns), so the
/// difference between a write that landed and a write that was swallowed is
/// visible in the reply and nowhere else.
///
/// Keys are parsed by ConfigKey::parseCommaSeparated, the same parser a skin
/// file's ConfigKey attribute goes through, so a key spelled correctly here is
/// spelled correctly in a skin.
class ControlSocket : public QObject {
    Q_OBJECT
  public:
    /// The port BITEDJ_CONTROL asks for, or nullopt when the variable is unset
    /// (the normal case) or does not parse as a port (logged as a fault).
    static std::optional<std::uint16_t> configuredPort();

    explicit ControlSocket(std::uint16_t port, QObject* parent = nullptr);
    ~ControlSocket() override;

  private slots:
    void onNewConnection();
    void onReadyRead();
    void onDisconnected();

  private:
    QString handleLine(const QString& line) const;

    QTcpServer* m_pServer;
    /// Partial input per client. A request is only acted on once its newline
    /// arrives, because a TCP read is not a message boundary.
    QHash<QTcpSocket*, QByteArray> m_buffers;
};
