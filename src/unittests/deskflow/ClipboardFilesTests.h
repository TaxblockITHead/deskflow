/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "base/Log.h"

#include <QTest>

class ClipboardFilesTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void packAndUnpackFilesAndFolders();
  void packSkipsFilesOverLimit();
  void packMissingFileGivesNothing();
  void unpackRejectsPathTraversal();
  void unpackRejectsTruncatedData();
  void unpackRejectsUnknownData();
  void sanitizeWindowsNames();
  void receiveReusesIdenticalPayload();
  void clipboardMarshallsFiles();
  void clipboardWithoutFilesHidesFiles();

private:
  Log m_log;
};
