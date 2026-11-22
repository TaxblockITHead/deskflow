/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QElapsedTimer>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <mutex>
#include <optional>

class QUdpSocket;

//! Finding the server again after its IP address changed
/*!
When a computer moves to another network (Wi-Fi changed, DHCP gave a new
address) the client can no longer reach the server at the address it was
configured with. The client then asks every network it is on, over UDP
broadcast on the Deskflow port, for the server it trusts. The server
answers only when asked for its own TLS certificate fingerprint, and the
client connects to the address the answer came from. The TLS connection
still verifies the certificate, so a forged answer can't redirect the
client to another server.

Messages are JSON objects in a single datagram:
\code
  request: {"deskflow": "discover", "version": 1, "fingerprints": ["v2:sha256:<hex>", ...]}
  reply:   {"deskflow": "server",   "version": 1, "fingerprint": "v2:sha256:<hex>",
            "port": 24800, "name": "<computer name>"}
\endcode
*/
namespace deskflow::discovery {

inline constexpr int kProtocolVersion = 1;
inline constexpr qsizetype kMaxDatagramSize = 4096;
inline constexpr qsizetype kMaxFingerprints = 32;
inline constexpr qsizetype kMaxNameLength = 255;
inline constexpr qint64 kMinSearchIntervalMs = 2000;

//! A server that answered a discovery request
struct ServerInfo
{
  QHostAddress address;
  quint16 port = 0;
  QString name;
  QString fingerprint;
};

//! Build a request for the servers with one of \p fingerprints
QByteArray makeRequest(const QStringList &fingerprints);

//! Fingerprints asked for in a request, or nothing if it isn't a valid request
std::optional<QStringList> parseRequest(const QByteArray &datagram);

//! Build the reply of a server
QByteArray makeReply(const QString &fingerprint, quint16 port, const QString &name);

//! Parse a reply; the address is left empty, it is the sender of the datagram
std::optional<ServerInfo> parseReply(const QByteArray &datagram);

//! True if \p a and \p b are the same fingerprint (in database line format)
bool sameFingerprint(const QString &a, const QString &b);

//! Fingerprints of the servers this client looks for
/*!
The server this client last connected to, or all trusted servers if it
hasn't connected to one yet.
*/
QStringList expectedServerFingerprints();

//! IPv4 broadcast addresses of all active network interfaces
QList<QHostAddress> broadcastAddresses();

//! Answers discovery requests on the server
/*!
Must be created in a thread with a running Qt event loop (the core's main
thread). start() and stop() may be called from any thread.
*/
class Responder : public QObject
{
  Q_OBJECT

public:
  explicit Responder(QObject *parent = nullptr);
  ~Responder() override;

  //! Listen for requests on UDP \p port and answer for this server
  void start(quint16 port, const QString &name, const QString &fingerprint);

  //! Stop answering requests
  void stop();

  //! UDP port requests are received on, 0 when not listening
  quint16 listeningPort() const;

Q_SIGNALS:
  void listening();

private:
  void doStart(quint16 port, const QString &name, const QString &fingerprint);
  void doStop();
  void readPending();

  QUdpSocket *m_socket = nullptr;
  QString m_name;
  QString m_fingerprint;
  quint16 m_tcpPort = 0;
  mutable std::mutex m_mutex;
  quint16 m_listeningPort = 0;
};

//! Looks for the server on the client
/*!
Must be created in a thread with a running Qt event loop (the core's main
thread). search() and takeResult() may be called from any thread.
*/
class Finder : public QObject
{
  Q_OBJECT

public:
  explicit Finder(QObject *parent = nullptr);
  ~Finder() override;

  //! Ask the networks for a server with one of \p fingerprints on \p port
  /*!
  Ignored while an earlier search is less than kMinSearchIntervalMs old.
  */
  void search(const QStringList &fingerprints, quint16 port);

  //! The server found since the last call, if any
  std::optional<ServerInfo> takeResult();

  //! Send requests to \p targets instead of the broadcast addresses (tests)
  void setTargets(const QList<QHostAddress> &targets);

Q_SIGNALS:
  void serverFound();

private:
  void doSearch(const QStringList &fingerprints, quint16 port);
  void readPending();

  QUdpSocket *m_socket = nullptr;
  QStringList m_wanted;
  QElapsedTimer m_lastSearch;
  QList<QHostAddress> m_targets;
  std::mutex m_mutex;
  std::optional<ServerInfo> m_result;
};

} // namespace deskflow::discovery
