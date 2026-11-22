/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/ServerDiscovery.h"

#include "base/Log.h"
#include "common/Settings.h"
#include "net/Fingerprint.h"
#include "net/FingerprintDatabase.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QUdpSocket>

#include <algorithm>

namespace deskflow::discovery {

namespace {

const auto kKeyType = QStringLiteral("deskflow");
const auto kKeyVersion = QStringLiteral("version");
const auto kKeyFingerprints = QStringLiteral("fingerprints");
const auto kKeyFingerprint = QStringLiteral("fingerprint");
const auto kKeyPort = QStringLiteral("port");
const auto kKeyName = QStringLiteral("name");
const auto kTypeRequest = QStringLiteral("discover");
const auto kTypeReply = QStringLiteral("server");

std::optional<QJsonObject> parseMessage(const QByteArray &datagram, const QString &type)
{
  if (datagram.isEmpty() || datagram.size() > kMaxDatagramSize) {
    return std::nullopt;
  }

  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(datagram, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) {
    return std::nullopt;
  }

  const auto object = document.object();
  // newer versions must keep the fields of version 1
  if (object.value(kKeyType).toString() != type || object.value(kKeyVersion).toInt() < kProtocolVersion) {
    return std::nullopt;
  }
  return object;
}

bool isValidFingerprint(const QString &line)
{
  return Fingerprint::fromDbLine(line).isValid();
}

QStringList validFingerprints(const FingerprintDatabase &db)
{
  QStringList result;
  for (const auto &fingerprint : db.fingerprints()) {
    if (fingerprint.isValid()) {
      result << fingerprint.toDbLine();
    }
  }
  return result;
}

} // namespace

QByteArray makeRequest(const QStringList &fingerprints)
{
  QJsonArray list;
  for (const auto &fingerprint : fingerprints.mid(0, kMaxFingerprints)) {
    list.append(fingerprint);
  }
  const QJsonObject object{{kKeyType, kTypeRequest}, {kKeyVersion, kProtocolVersion}, {kKeyFingerprints, list}};
  return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

std::optional<QStringList> parseRequest(const QByteArray &datagram)
{
  const auto object = parseMessage(datagram, kTypeRequest);
  if (!object) {
    return std::nullopt;
  }

  const auto list = object->value(kKeyFingerprints).toArray();
  if (list.isEmpty() || list.size() > kMaxFingerprints) {
    return std::nullopt;
  }

  QStringList fingerprints;
  for (const auto &value : list) {
    if (const auto fingerprint = value.toString(); isValidFingerprint(fingerprint)) {
      fingerprints << fingerprint;
    }
  }
  if (fingerprints.isEmpty()) {
    return std::nullopt;
  }
  return fingerprints;
}

QByteArray makeReply(const QString &fingerprint, quint16 port, const QString &name)
{
  const QJsonObject object{
      {kKeyType, kTypeReply},
      {kKeyVersion, kProtocolVersion},
      {kKeyFingerprint, fingerprint},
      {kKeyPort, static_cast<int>(port)},
      {kKeyName, name.left(kMaxNameLength)}
  };
  return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

std::optional<ServerInfo> parseReply(const QByteArray &datagram)
{
  const auto object = parseMessage(datagram, kTypeReply);
  if (!object) {
    return std::nullopt;
  }

  const auto fingerprint = object->value(kKeyFingerprint).toString();
  const auto port = object->value(kKeyPort).toInt();
  if (!isValidFingerprint(fingerprint) || port <= 0 || port > 65535) {
    return std::nullopt;
  }

  ServerInfo info;
  info.port = static_cast<quint16>(port);
  info.fingerprint = fingerprint;
  info.name = object->value(kKeyName).toString().left(kMaxNameLength);
  return info;
}

bool sameFingerprint(const QString &a, const QString &b)
{
  const auto first = Fingerprint::fromDbLine(a);
  return first.isValid() && first == Fingerprint::fromDbLine(b);
}

QStringList expectedServerFingerprints()
{
  FingerprintDatabase last;
  last.read(Settings::tlsLastServerDb());
  if (auto result = validFingerprints(last); !result.isEmpty()) {
    return result;
  }

  FingerprintDatabase trusted;
  trusted.read(Settings::tlsTrustedServersDb());
  return validFingerprints(trusted).mid(0, kMaxFingerprints);
}

QList<QHostAddress> broadcastAddresses()
{
  QList<QHostAddress> result;
  const auto interfaces = QNetworkInterface::allInterfaces();
  for (const auto &netInterface : interfaces) {
    const auto flags = netInterface.flags();
    if (!flags.testFlag(QNetworkInterface::IsUp) || !flags.testFlag(QNetworkInterface::IsRunning) ||
        !flags.testFlag(QNetworkInterface::CanBroadcast) || flags.testFlag(QNetworkInterface::IsLoopBack)) {
      continue;
    }
    for (const auto &entry : netInterface.addressEntries()) {
      const auto broadcast = entry.broadcast();
      if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && !broadcast.isNull() &&
          !result.contains(broadcast)) {
        result << broadcast;
      }
    }
  }
  if (!result.contains(QHostAddress(QHostAddress::Broadcast))) {
    result << QHostAddress(QHostAddress::Broadcast);
  }
  return result;
}

//
// Responder
//

Responder::Responder(QObject *parent) : QObject(parent)
{
  // do nothing
}

Responder::~Responder() = default;

void Responder::start(quint16 port, const QString &name, const QString &fingerprint)
{
  QMetaObject::invokeMethod(
      this, [this, port, name, fingerprint] { doStart(port, name, fingerprint); }, Qt::QueuedConnection
  );
}

void Responder::stop()
{
  QMetaObject::invokeMethod(this, [this] { doStop(); }, Qt::QueuedConnection);
}

quint16 Responder::listeningPort() const
{
  std::scoped_lock lock{m_mutex};
  return m_listeningPort;
}

void Responder::doStart(quint16 port, const QString &name, const QString &fingerprint)
{
  doStop();

  m_name = name;
  m_fingerprint = fingerprint;
  m_tcpPort = port;

  m_socket = new QUdpSocket(this);
  if (!m_socket->bind(QHostAddress::AnyIPv4, port, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
    LOG_WARN(
        "unable to listen for clients looking for this server on udp port %d: %s", port,
        qPrintable(m_socket->errorString())
    );
    delete m_socket;
    m_socket = nullptr;
    return;
  }

  connect(m_socket, &QUdpSocket::readyRead, this, &Responder::readPending);
  {
    std::scoped_lock lock{m_mutex};
    m_listeningPort = m_socket->localPort();
  }
  LOG_DEBUG("answering clients looking for this server on udp port %d", m_socket->localPort());
  Q_EMIT listening();
}

void Responder::doStop()
{
  if (m_socket == nullptr) {
    return;
  }
  delete m_socket;
  m_socket = nullptr;
  std::scoped_lock lock{m_mutex};
  m_listeningPort = 0;
}

void Responder::readPending()
{
  while (m_socket != nullptr && m_socket->hasPendingDatagrams()) {
    const auto datagram = m_socket->receiveDatagram(kMaxDatagramSize + 1);
    const auto wanted = parseRequest(datagram.data());
    if (!wanted) {
      continue;
    }

    const bool askedForUs = std::any_of(wanted->begin(), wanted->end(), [this](const QString &fingerprint) {
      return sameFingerprint(fingerprint, m_fingerprint);
    });
    if (!askedForUs) {
      continue;
    }

    LOG_DEBUG("client %s is looking for this server, answering", qPrintable(datagram.senderAddress().toString()));
    m_socket->writeDatagram(
        makeReply(m_fingerprint, m_tcpPort, m_name), datagram.senderAddress(),
        static_cast<quint16>(datagram.senderPort())
    );
  }
}

//
// Finder
//

Finder::Finder(QObject *parent) : QObject(parent)
{
  // do nothing
}

Finder::~Finder() = default;

void Finder::search(const QStringList &fingerprints, quint16 port)
{
  QMetaObject::invokeMethod(this, [this, fingerprints, port] { doSearch(fingerprints, port); }, Qt::QueuedConnection);
}

std::optional<ServerInfo> Finder::takeResult()
{
  std::scoped_lock lock{m_mutex};
  auto result = m_result;
  m_result.reset();
  return result;
}

void Finder::setTargets(const QList<QHostAddress> &targets)
{
  m_targets = targets;
}

void Finder::doSearch(const QStringList &fingerprints, quint16 port)
{
  if (fingerprints.isEmpty() || (m_lastSearch.isValid() && m_lastSearch.elapsed() < kMinSearchIntervalMs)) {
    return;
  }
  m_lastSearch.start();
  m_wanted = fingerprints;

  if (m_socket == nullptr) {
    m_socket = new QUdpSocket(this);
    if (!m_socket->bind(QHostAddress::AnyIPv4, 0)) {
      LOG_WARN("unable to look for the server: %s", qPrintable(m_socket->errorString()));
      delete m_socket;
      m_socket = nullptr;
      return;
    }
    connect(m_socket, &QUdpSocket::readyRead, this, &Finder::readPending);
  }

  const auto request = makeRequest(fingerprints);
  const auto targets = m_targets.isEmpty() ? broadcastAddresses() : m_targets;
  LOG_DEBUG("looking for the server on %lld network(s)", static_cast<long long>(targets.size()));
  for (const auto &target : targets) {
    if (m_socket->writeDatagram(request, target, port) < 0) {
      LOG_DEBUG(
          "unable to look for the server on %s: %s", qPrintable(target.toString()), qPrintable(m_socket->errorString())
      );
    }
  }
}

void Finder::readPending()
{
  while (m_socket != nullptr && m_socket->hasPendingDatagrams()) {
    const auto datagram = m_socket->receiveDatagram(kMaxDatagramSize + 1);
    auto info = parseReply(datagram.data());
    if (!info) {
      continue;
    }

    const bool isWanted = std::any_of(m_wanted.begin(), m_wanted.end(), [&info](const QString &fingerprint) {
      return sameFingerprint(fingerprint, info->fingerprint);
    });
    if (!isWanted) {
      continue;
    }

    // a reply can arrive as an IPv4-mapped IPv6 address on some systems
    QHostAddress address = datagram.senderAddress();
    bool isIpv4 = false;
    if (const auto ipv4 = address.toIPv4Address(&isIpv4); isIpv4) {
      address = QHostAddress(ipv4);
    }
    info->address = address;

    LOG_DEBUG(
        "server \"%s\" answered from %s port %d", qPrintable(info->name), qPrintable(address.toString()), info->port
    );
    {
      std::scoped_lock lock{m_mutex};
      m_result = info;
    }
    Q_EMIT serverFound();
  }
}

} // namespace deskflow::discovery
