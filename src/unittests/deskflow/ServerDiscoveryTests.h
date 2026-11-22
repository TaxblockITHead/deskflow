/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "base/Log.h"

#include <QTest>

class ServerDiscoveryTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void requestRoundTrip();
  void requestRejectsInvalidData();
  void requestLimitsFingerprints();
  void replyRoundTrip();
  void replyRejectsInvalidData();
  void sameFingerprint();
  void broadcastAddressesIncludeGlobalBroadcast();
  void responderAnswersFinder();
  void responderIgnoresOtherServers();
  void finderIgnoresUnexpectedServers();

private:
  Log m_log;
};
