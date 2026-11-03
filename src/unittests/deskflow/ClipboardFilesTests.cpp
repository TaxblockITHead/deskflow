/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ClipboardFilesTests.h"

#include "deskflow/Clipboard.h"
#include "deskflow/ClipboardFiles.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

namespace {

void writeFile(const QString &path, const QByteArray &content)
{
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly));
  QVERIFY(file.write(content) == content.size());
}

QByteArray readFile(const QString &path)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return {};
  }
  return file.readAll();
}

std::string utf8(const QString &s)
{
  return s.toUtf8().toStdString();
}

void appendU32(std::string &out, uint32_t v)
{
  for (int shift = 24; shift >= 0; shift -= 8) {
    out += static_cast<char>((v >> shift) & 0xff);
  }
}

void appendU64(std::string &out, uint64_t v)
{
  for (int shift = 56; shift >= 0; shift -= 8) {
    out += static_cast<char>((v >> shift) & 0xff);
  }
}

std::string singleFilePayload(const std::string &path, const std::string &content)
{
  std::string out = "DFF1";
  appendU32(out, 1);
  out += static_cast<char>(0); // file
  appendU32(out, static_cast<uint32_t>(path.size()));
  out += path;
  appendU64(out, content.size());
  out += content;
  return out;
}

} // namespace

void ClipboardFilesTests::initTestCase()
{
  m_log.setFilter(LogLevel::Level::Verbose);
}

void ClipboardFilesTests::packAndUnpackFilesAndFolders()
{
  QTemporaryDir source;
  QTemporaryDir target;
  QVERIFY(source.isValid());
  QVERIFY(target.isValid());

  const QByteArray binary("pdf\0data\xff\x01", 10);
  writeFile(source.filePath("report.pdf"), binary);
  writeFile(source.filePath("folder/sheet.xlsx"), "sheet");
  writeFile(source.filePath("folder/inner/archive.zip"), "zip");
  writeFile(source.filePath("folder/.DS_Store"), "finder");
  QVERIFY(QDir().mkpath(source.filePath("folder/empty")));

  const std::vector<std::string> paths = {utf8(source.filePath("report.pdf")), utf8(source.filePath("folder"))};
  const std::string payload = ClipboardFiles::pack(paths, 1024 * 1024);
  QVERIFY(!payload.empty());

  const auto received = ClipboardFiles::unpack(payload, utf8(target.path()));
  QCOMPARE(received.size(), size_t{2});
  QCOMPARE(QString::fromStdString(received[0]), target.filePath("report.pdf"));
  QCOMPARE(QString::fromStdString(received[1]), target.filePath("folder"));

  QCOMPARE(readFile(target.filePath("report.pdf")), binary);
  QCOMPARE(readFile(target.filePath("folder/sheet.xlsx")), QByteArray("sheet"));
  QCOMPARE(readFile(target.filePath("folder/inner/archive.zip")), QByteArray("zip"));
  QVERIFY(QFileInfo(target.filePath("folder/empty")).isDir());
  QVERIFY(!QFileInfo::exists(target.filePath("folder/.DS_Store")));
}

void ClipboardFilesTests::packSkipsFilesOverLimit()
{
  QTemporaryDir source;
  QVERIFY(source.isValid());
  writeFile(source.filePath("big.bin"), QByteArray(2048, 'x'));

  const std::vector<std::string> paths = {utf8(source.filePath("big.bin"))};
  QVERIFY(ClipboardFiles::pack(paths, 1024).empty());
  QVERIFY(!ClipboardFiles::pack(paths, 4096).empty());
}

void ClipboardFilesTests::packMissingFileGivesNothing()
{
  QTemporaryDir source;
  QVERIFY(source.isValid());
  const std::vector<std::string> paths = {utf8(source.filePath("missing.pdf"))};
  QVERIFY(ClipboardFiles::pack(paths, 1024).empty());
}

void ClipboardFilesTests::unpackRejectsPathTraversal()
{
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const QString target = root.filePath("target");

  QVERIFY(ClipboardFiles::unpack(singleFilePayload("../evil.txt", "x"), utf8(target)).empty());
  QVERIFY(ClipboardFiles::unpack(singleFilePayload("a/../../evil.txt", "x"), utf8(target)).empty());
  QVERIFY(ClipboardFiles::unpack(singleFilePayload("/etc/evil.txt", "x"), utf8(target)).empty());
  QVERIFY(!QFileInfo::exists(root.filePath("evil.txt")));

  // a backslash is not a separator, it becomes part of the name
  const auto received = ClipboardFiles::unpack(singleFilePayload("..\\evil.txt", "x"), utf8(target));
  QCOMPARE(received.size(), size_t{1});
  QVERIFY(QFileInfo::exists(QDir(target).filePath(".._evil.txt")));
  QVERIFY(!QFileInfo::exists(root.filePath("evil.txt")));
}

void ClipboardFilesTests::unpackRejectsTruncatedData()
{
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const QString target = root.filePath("target");

  std::string payload = singleFilePayload("file.txt", "content");
  payload.resize(payload.size() - 3);

  QVERIFY(ClipboardFiles::unpack(payload, utf8(target)).empty());
  // the folder created for the transfer is removed again
  QVERIFY(!QFileInfo::exists(target));
}

void ClipboardFilesTests::unpackRejectsUnknownData()
{
  QTemporaryDir root;
  QVERIFY(root.isValid());
  QVERIFY(ClipboardFiles::unpack("not files", utf8(root.filePath("target"))).empty());
  QVERIFY(ClipboardFiles::unpack("", utf8(root.filePath("target"))).empty());

  std::string hugeCount = "DFF1";
  appendU32(hugeCount, 0xffffffff);
  QVERIFY(ClipboardFiles::unpack(hugeCount, utf8(root.filePath("target"))).empty());
}

void ClipboardFilesTests::sanitizeWindowsNames()
{
  QCOMPARE(ClipboardFiles::sanitizePathComponent("report.pdf"), "report.pdf");
  QCOMPARE(ClipboardFiles::sanitizePathComponent("a:b?c*.txt"), "a_b_c_.txt");
  QCOMPARE(ClipboardFiles::sanitizePathComponent("con.txt"), "_con.txt");
  QCOMPARE(ClipboardFiles::sanitizePathComponent("trailing. "), "trailing");
  QCOMPARE(ClipboardFiles::sanitizePathComponent("..."), "_");
  QCOMPARE(ClipboardFiles::sanitizePathComponent(".."), "");
  QCOMPARE(ClipboardFiles::sanitizePathComponent("."), "");
  QCOMPARE(ClipboardFiles::sanitizePathComponent(""), "");
}

void ClipboardFilesTests::receiveReusesIdenticalPayload()
{
  const std::string payload = singleFilePayload("received.txt", "same content");

  const auto first = ClipboardFiles::receive(payload);
  QCOMPARE(first.size(), size_t{1});
  QCOMPARE(readFile(QString::fromStdString(first[0])), QByteArray("same content"));
  QVERIFY(QString::fromStdString(first[0]).startsWith(QString::fromStdString(ClipboardFiles::receiveDirectory())));

  const auto second = ClipboardFiles::receive(payload);
  QVERIFY(second == first);

  const auto other = ClipboardFiles::receive(singleFilePayload("received.txt", "other content"));
  QCOMPARE(other.size(), size_t{1});
  QVERIFY(other != first);
}

void ClipboardFilesTests::clipboardMarshallsFiles()
{
  using enum IClipboard::Format;
  const std::string payload = singleFilePayload("a.txt", "a");

  Clipboard source;
  QVERIFY(source.open(0));
  QVERIFY(source.empty());
  source.add(Text, "a.txt");
  source.add(Files, payload);
  source.close();

  Clipboard copy;
  copy.unmarshall(source.marshall(), 0);
  QVERIFY(copy.open(0));
  QVERIFY(copy.has(Files));
  QCOMPARE(copy.get(Files), payload);
  QCOMPARE(copy.get(Text), "a.txt");
  copy.close();

  QVERIFY(ClipboardFiles::hasFiles(&copy));
}

void ClipboardFilesTests::clipboardWithoutFilesHidesFiles()
{
  using enum IClipboard::Format;

  Clipboard source;
  QVERIFY(source.open(0));
  QVERIFY(source.empty());
  source.add(Text, "text");
  source.add(Files, singleFilePayload("a.txt", "a"));
  source.close();

  ClipboardWithoutFiles withoutFiles(source);
  Clipboard copy;
  QVERIFY(Clipboard::copy(&copy, &withoutFiles));

  QVERIFY(copy.open(0));
  QVERIFY(copy.has(Text));
  QVERIFY(!copy.has(Files));
  copy.close();
  QVERIFY(!ClipboardFiles::hasFiles(&copy));
}

QTEST_MAIN(ClipboardFilesTests)
