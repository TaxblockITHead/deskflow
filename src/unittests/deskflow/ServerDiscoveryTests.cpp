/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ServerDiscoveryTests.h"

#include "deskflow/ServerDiscovery.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QSignalSpy>
#include <QUdpSocket>

using namespace deskflow::discovery;

namespace {

QString fingerprint(char fill)
{
  return QStringLiteral("v2:sha256:%1").arg(QString::fromLatin1(QByteArray(32, fill).toHex()));
}

// a free udp port on this computer, for a responder to listen on
quint16 freeUdpPort()
{
  QUdpSocket probe;
  if (!probe.bind(QHostAddress::AnyIPv4, 0)) {
    return 0;
  }
  return probe.localPort();
}

QByteArray json(const QJsonObject &object)
{
  return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

} // namespace

void ServerDiscoveryTests::initTestCase()
{
  m_log.setFilter(LogLevel::Level::Debug);
}

void ServerDiscoveryTests::requestRoundTrip()
{
  const QStringList wanted = {fingerprint('\x11'), fingerprint('\x22')};
  const auto parsed = parseRequest(makeRequest(wanted));
  QVERIFY(parsed.has_value());
  QCOMPARE(*parsed, wanted);
}

void ServerDiscoveryTests::requestRejectsInvalidData()
{
  QVERIFY(!parseRequest({}).has_value());
  QVERIFY(!parseRequest("not json").has_value());
  QVERIFY(!parseRequest("[1, 2]").has_value());
  QVERIFY(!parseRequest(QByteArray(kMaxDatagramSize + 1, ' ')).has_value());

  // a reply is not a request
  QVERIFY(!parseRequest(makeReply(fingerprint('\x11'), 24800, "server")).has_value());

  // no or only invalid fingerprints
  QVERIFY(!parseRequest(makeRequest({})).has_value());
  QVERIFY(!parseRequest(makeRequest({QStringLiteral("v2:sha256:abcd")})).has_value());

  // unknown older version
  QVERIFY(!parseRequest(json({{"deskflow", "discover"}, {"version", 0}, {"fingerprints", "x"}})).has_value());
}

void ServerDiscoveryTests::requestLimitsFingerprints()
{
  QStringList many;
  for (int i = 0; i < kMaxFingerprints + 10; ++i) {
    many << fingerprint(static_cast<char>(i + 1));
  }
  const auto parsed = parseRequest(makeRequest(many));
  QVERIFY(parsed.has_value());
  QCOMPARE(parsed->size(), kMaxFingerprints);
}

void ServerDiscoveryTests::replyRoundTrip()
{
  const auto parsed = parseReply(makeReply(fingerprint('\x11'), 24800, "DESKTOP-0AQN90E"));
  QVERIFY(parsed.has_value());
  QCOMPARE(parsed->fingerprint, fingerprint('\x11'));
  QCOMPARE(parsed->port, quint16{24800});
  QCOMPARE(parsed->name, QStringLiteral("DESKTOP-0AQN90E"));
  QVERIFY(parsed->address.isNull());
}

void ServerDiscoveryTests::replyRejectsInvalidData()
{
  QVERIFY(!parseReply({}).has_value());
  QVERIFY(!parseReply("{").has_value());
  QVERIFY(!parseReply(makeRequest({fingerprint('\x11')})).has_value());
  QVERIFY(!parseReply(makeReply(fingerprint('\x11'), 0, "server")).has_value());
  QVERIFY(!parseReply(makeReply(QStringLiteral("nonsense"), 24800, "server")).has_value());
  QVERIFY(
      !parseReply(json({{"deskflow", "server"}, {"version", 1}, {"fingerprint", fingerprint('\x11')}, {"port", 70000}}))
           .has_value()
  );

  // long names are cut
  const auto parsed = parseReply(makeReply(fingerprint('\x11'), 24800, QString(1000, 'x')));
  QVERIFY(parsed.has_value());
  QCOMPARE(parsed->name.size(), kMaxNameLength);
}

void ServerDiscoveryTests::sameFingerprint()
{
  QVERIFY(deskflow::discovery::sameFingerprint(fingerprint('\x11'), fingerprint('\x11')));
  const auto upperHex =
      QStringLiteral("v2:sha256:%1").arg(QString::fromLatin1(QByteArray(32, '\x11').toHex().toUpper()));
  QVERIFY(deskflow::discovery::sameFingerprint(fingerprint('\x11'), upperHex));
  QVERIFY(!deskflow::discovery::sameFingerprint(fingerprint('\x11'), fingerprint('\x22')));
  QVERIFY(!deskflow::discovery::sameFingerprint(QStringLiteral("x"), QStringLiteral("x")));
}

void ServerDiscoveryTests::broadcastAddressesIncludeGlobalBroadcast()
{
  const auto addresses = broadcastAddresses();
  QVERIFY(addresses.contains(QHostAddress(QHostAddress::Broadcast)));
  for (const auto &address : addresses) {
    QCOMPARE(address.protocol(), QAbstractSocket::IPv4Protocol);
  }
}

void ServerDiscoveryTests::responderAnswersFinder()
{
  const auto port = freeUdpPort();
  QVERIFY(port != 0);

  Responder responder;
  responder.start(port, QStringLiteral("server"), fingerprint('\x11'));
  QTRY_COMPARE(responder.listeningPort(), port);

  Finder finder;
  finder.setTargets({QHostAddress(QHostAddress::LocalHost)});
  QSignalSpy foundSpy(&finder, &Finder::serverFound);
  finder.search({fingerprint('\x22'), fingerprint('\x11')}, port);

  QVERIFY(foundSpy.wait(5000));
  const auto found = finder.takeResult();
  QVERIFY(found.has_value());
  QCOMPARE(found->address, QHostAddress(QHostAddress::LocalHost));
  QCOMPARE(found->port, port);
  QCOMPARE(found->name, QStringLiteral("server"));
  QVERIFY(deskflow::discovery::sameFingerprint(found->fingerprint, fingerprint('\x11')));

  // the result is handed out once
  QVERIFY(!finder.takeResult().has_value());

  responder.stop();
  QTRY_COMPARE(responder.listeningPort(), quint16{0});
}

void ServerDiscoveryTests::responderIgnoresOtherServers()
{
  const auto port = freeUdpPort();
  QVERIFY(port != 0);

  Responder responder;
  responder.start(port, QStringLiteral("server"), fingerprint('\x11'));
  QTRY_COMPARE(responder.listeningPort(), port);

  Finder finder;
  finder.setTargets({QHostAddress(QHostAddress::LocalHost)});
  finder.search({fingerprint('\x33')}, port);

  QTest::qWait(500);
  QVERIFY(!finder.takeResult().has_value());
}

void ServerDiscoveryTests::finderIgnoresUnexpectedServers()
{
  const auto port = freeUdpPort();
  QVERIFY(port != 0);

  // a server that answers any request with a fingerprint nobody asked for
  QUdpSocket impostor;
  QVERIFY(impostor.bind(QHostAddress::AnyIPv4, port));
  QObject::connect(&impostor, &QUdpSocket::readyRead, &impostor, [&impostor] {
    while (impostor.hasPendingDatagrams()) {
      const auto datagram = impostor.receiveDatagram();
      impostor.writeDatagram(
          makeReply(fingerprint('\x44'), 24800, "impostor"), datagram.senderAddress(),
          static_cast<quint16>(datagram.senderPort())
      );
    }
  });

  Finder finder;
  finder.setTargets({QHostAddress(QHostAddress::LocalHost)});
  finder.search({fingerprint('\x11')}, port);

  QTest::qWait(500);
  QVERIFY(!finder.takeResult().has_value());
}

QTEST_MAIN(ServerDiscoveryTests)
