/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/ClipboardFiles.h"

#include "base/Log.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string_view>

namespace {

const char kMagic[] = {'D', 'F', 'F', '1'};
const size_t kMagicSize = sizeof(kMagic);

enum class EntryKind : uint8_t
{
  File = 0,
  Directory = 1
};

// smallest possible entry: kind + path length + 1 byte path
const size_t kMinEntrySize = 1 + 4 + 1;
const uint32_t kMaxPathBytes = 32 * 1024;

// leave room for the other clipboard formats (e.g. the file names as text)
const uint64_t kOtherFormatsAllowance = 64 * 1024;

struct Entry
{
  EntryKind kind;
  QString relativePath;
  QString sourcePath;
  uint64_t size = 0;
};

std::atomic<uint64_t> s_maxPayloadBytes{ClipboardFiles::kHardLimitBytes};

std::mutex s_packMutex;
QString s_packKey;
std::string s_packed;

std::mutex s_receiveMutex;
QByteArray s_receivedHash;
std::vector<std::string> s_receivedPaths;

QString fromUtf8(const std::string &s)
{
  return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

std::string toUtf8(const QString &s)
{
  return s.toUtf8().toStdString();
}

void writeU8(std::string &out, uint8_t v)
{
  out += static_cast<char>(v);
}

void writeU32(std::string &out, uint32_t v)
{
  for (int shift = 24; shift >= 0; shift -= 8) {
    out += static_cast<char>((v >> shift) & 0xff);
  }
}

void writeU64(std::string &out, uint64_t v)
{
  for (int shift = 56; shift >= 0; shift -= 8) {
    out += static_cast<char>((v >> shift) & 0xff);
  }
}

class Reader
{
public:
  explicit Reader(const std::string &data) : m_data(data)
  {
    // do nothing
  }

  size_t remaining() const
  {
    return m_data.size() - m_pos;
  }

  bool readBytes(size_t n, std::string_view &out)
  {
    if (n > remaining()) {
      return false;
    }
    out = std::string_view(m_data).substr(m_pos, n);
    m_pos += n;
    return true;
  }

  bool readU8(uint8_t &v)
  {
    std::string_view bytes;
    if (!readBytes(1, bytes)) {
      return false;
    }
    v = static_cast<uint8_t>(bytes[0]);
    return true;
  }

  bool readU32(uint32_t &v)
  {
    std::string_view bytes;
    if (!readBytes(4, bytes)) {
      return false;
    }
    v = 0;
    for (char c : bytes) {
      v = (v << 8) | static_cast<uint8_t>(c);
    }
    return true;
  }

  bool readU64(uint64_t &v)
  {
    std::string_view bytes;
    if (!readBytes(8, bytes)) {
      return false;
    }
    v = 0;
    for (char c : bytes) {
      v = (v << 8) | static_cast<uint8_t>(c);
    }
    return true;
  }

private:
  const std::string &m_data;
  size_t m_pos = 0;
};

QString topLevelName(const QFileInfo &info)
{
  QString name = info.fileName();
  if (name.isEmpty()) {
    // path ends with a separator, or is a drive root such as "C:/"
    name = QDir(info.absoluteFilePath()).dirName();
  }
  if (name.isEmpty()) {
    name = QStringLiteral("files");
  }
  return name;
}

bool isIgnored(const QFileInfo &info)
{
  // Finder metadata, recreated by the receiving Finder if needed
  return info.fileName() == QStringLiteral(".DS_Store");
}

void collectDirectory(const QString &dirPath, const QString &relativePath, std::vector<Entry> &entries, uint64_t &total)
{
  const auto filters = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System;
  const QFileInfoList children = QDir(dirPath).entryInfoList(filters, QDir::Name);
  for (const QFileInfo &child : children) {
    // links are skipped: they could point outside the copied folder or loop
    if (child.isSymLink() || isIgnored(child)) {
      continue;
    }
    const QString childRelative = relativePath + QLatin1Char('/') + child.fileName();
    if (child.isDir()) {
      entries.push_back({EntryKind::Directory, childRelative, child.absoluteFilePath(), 0});
      collectDirectory(child.absoluteFilePath(), childRelative, entries, total);
    } else if (child.isFile()) {
      const auto size = static_cast<uint64_t>(child.size());
      entries.push_back({EntryKind::File, childRelative, child.absoluteFilePath(), size});
      total += size;
    }
  }
}

std::vector<Entry> collect(const std::vector<std::string> &paths, uint64_t &total)
{
  std::vector<Entry> entries;
  total = 0;
  for (const auto &path : paths) {
    // QFileInfo follows a link given at the top level, which is what was copied
    const QFileInfo info(fromUtf8(path));
    if (!info.exists()) {
      LOG_WARN("copied file does not exist: %s", path.c_str());
      continue;
    }
    const QString name = topLevelName(info);
    if (info.isDir()) {
      entries.push_back({EntryKind::Directory, name, info.absoluteFilePath(), 0});
      collectDirectory(info.absoluteFilePath(), name, entries, total);
    } else if (info.isFile()) {
      const auto size = static_cast<uint64_t>(info.size());
      entries.push_back({EntryKind::File, name, info.absoluteFilePath(), size});
      total += size;
    }
  }
  return entries;
}

QString cacheKey(const std::vector<Entry> &entries, uint64_t maxBytes)
{
  QString key = QString::number(maxBytes);
  for (const auto &entry : entries) {
    key += QLatin1Char('\n') + entry.sourcePath + QLatin1Char('|') + QString::number(entry.size);
    if (entry.kind == EntryKind::File) {
      key += QLatin1Char('|') + QString::number(QFileInfo(entry.sourcePath).lastModified().toMSecsSinceEpoch());
    }
  }
  return key;
}

std::string packEntries(const std::vector<Entry> &entries, uint64_t total, uint64_t maxBytes)
{
  std::string out;
  out.reserve(static_cast<size_t>(total) + entries.size() * 64 + 16);
  out.append(kMagic, kMagicSize);
  writeU32(out, static_cast<uint32_t>(entries.size()));

  for (const auto &entry : entries) {
    // macOS file names are decomposed (NFD), other systems expect composed
    const QByteArray path = entry.relativePath.normalized(QString::NormalizationForm_C).toUtf8();
    writeU8(out, static_cast<uint8_t>(entry.kind));
    writeU32(out, static_cast<uint32_t>(path.size()));
    out.append(path.constData(), static_cast<size_t>(path.size()));

    if (entry.kind == EntryKind::Directory) {
      continue;
    }

    QFile file(entry.sourcePath);
    if (!file.open(QIODevice::ReadOnly)) {
      LOG_WARN("not sharing copied files, unable to read: %s", toUtf8(entry.sourcePath).c_str());
      return {};
    }
    // the file may have changed since it was measured, so use what was read
    const QByteArray content = file.readAll();
    if (out.size() + static_cast<uint64_t>(content.size()) > maxBytes) {
      LOG_WARN("not sharing copied files, they grew beyond the clipboard size limit while being read");
      return {};
    }
    writeU64(out, static_cast<uint64_t>(content.size()));
    out.append(content.constData(), static_cast<size_t>(content.size()));
  }
  return out;
}

bool isReservedWindowsName(const QString &component)
{
  // Windows refuses these names with any extension, e.g. "con.txt"
  const QString base = component.section(QLatin1Char('.'), 0, 0).toUpper();
  static const QStringList reserved = {QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),
                                       QStringLiteral("NUL"),  QStringLiteral("COM1"), QStringLiteral("COM2"),
                                       QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
                                       QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
                                       QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
                                       QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
                                       QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
                                       QStringLiteral("LPT9")};
  return reserved.contains(base);
}

QString sanitizeComponent(const QString &component)
{
  if (component.isEmpty() || component == QStringLiteral(".") || component == QStringLiteral("..")) {
    return {};
  }

  QString clean;
  clean.reserve(component.size());
  for (const QChar c : component) {
    // characters that are separators or invalid in a Windows file name
    static const QString invalid = QStringLiteral("<>:\"/\\|?*");
    if (c.unicode() < 0x20 || invalid.contains(c)) {
      clean += QLatin1Char('_');
    } else {
      clean += c;
    }
  }

  // Windows drops trailing dots and spaces, which could merge two names
  while (clean.endsWith(QLatin1Char('.')) || clean.endsWith(QLatin1Char(' '))) {
    clean.chop(1);
  }
  if (clean.isEmpty()) {
    clean = QStringLiteral("_");
  }
  if (isReservedWindowsName(clean)) {
    clean.prepend(QLatin1Char('_'));
  }
  return clean;
}

QString sanitizeRelativePath(const QString &raw)
{
  QStringList clean;
  for (const QString &component : raw.split(QLatin1Char('/'))) {
    const QString safe = sanitizeComponent(component);
    if (safe.isEmpty()) {
      return {};
    }
    clean << safe;
  }
  return clean.join(QLatin1Char('/'));
}

bool isInside(const QString &path, const QString &dir)
{
  const QString cleanPath = QDir::cleanPath(path);
  const QString cleanDir = QDir::cleanPath(dir);
  return cleanPath.startsWith(cleanDir + QLatin1Char('/'));
}

void removeOldTransfers(const QDir &receiveDir, int keep)
{
  QStringList transfers = receiveDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
  while (transfers.size() > keep) {
    const QString oldest = transfers.takeFirst();
    QDir(receiveDir.filePath(oldest)).removeRecursively();
    LOG_DEBUG("removed old received files: %s", toUtf8(oldest).c_str());
  }
}

} // namespace

void ClipboardFiles::setMaxPayloadBytes(uint64_t bytes)
{
  s_maxPayloadBytes = std::min(bytes, kHardLimitBytes);
}

uint64_t ClipboardFiles::maxPayloadBytes()
{
  return s_maxPayloadBytes;
}

std::string ClipboardFiles::pack(const std::vector<std::string> &paths)
{
  const uint64_t limit = maxPayloadBytes();
  const uint64_t maxBytes = limit > kOtherFormatsAllowance ? limit - kOtherFormatsAllowance : limit;

  uint64_t total = 0;
  const auto entries = collect(paths, total);
  if (entries.empty()) {
    return {};
  }

  if (total > maxBytes) {
    LOG_WARN(
        "not sharing copied files, total size %llu KB is over the clipboard size limit %llu KB, "
        "increase the clipboard size limit in the server settings to share them",
        static_cast<unsigned long long>(total / 1024), static_cast<unsigned long long>(maxBytes / 1024)
    );
    return {};
  }

  std::scoped_lock lock{s_packMutex};
  const QString key = cacheKey(entries, maxBytes);
  if (key == s_packKey && !s_packed.empty()) {
    LOG_DEBUG("copied files unchanged, reusing packed data");
    return s_packed;
  }

  std::string packed = packEntries(entries, total, maxBytes);
  if (packed.empty()) {
    return {};
  }
  LOG_INFO(
      "sharing %zu copied file entries, %llu KB", entries.size(), static_cast<unsigned long long>(packed.size() / 1024)
  );

  s_packKey = key;
  s_packed = packed;
  return packed;
}

std::string ClipboardFiles::pack(const std::vector<std::string> &paths, uint64_t maxBytes)
{
  maxBytes = std::min(maxBytes, kHardLimitBytes);
  uint64_t total = 0;
  const auto entries = collect(paths, total);
  if (entries.empty() || total > maxBytes) {
    return {};
  }
  return packEntries(entries, total, maxBytes);
}

std::vector<std::string> ClipboardFiles::unpack(const std::string &payload, const std::string &targetDir)
{
  const QString target = QDir::cleanPath(QDir(fromUtf8(targetDir)).absolutePath());
  bool createdTarget = false;
  if (!QDir(target).exists()) {
    if (!QDir().mkpath(target)) {
      LOG_ERR("unable to create folder for received files: %s", toUtf8(target).c_str());
      return {};
    }
    createdTarget = true;
  }

  std::vector<std::string> topLevel;
  QStringList written;
  auto fail = [&](const char *reason) {
    LOG_ERR("unable to receive copied files: %s", reason);
    if (createdTarget) {
      QDir(target).removeRecursively();
    } else {
      for (const QString &path : written) {
        if (QFileInfo(path).isDir()) {
          QDir(path).removeRecursively();
        } else {
          QFile::remove(path);
        }
      }
    }
    return std::vector<std::string>{};
  };

  Reader reader(payload);
  std::string_view magic;
  if (!reader.readBytes(kMagicSize, magic) || magic != std::string_view(kMagic, kMagicSize)) {
    return fail("unknown data format");
  }

  uint32_t count = 0;
  if (!reader.readU32(count) || count > reader.remaining() / kMinEntrySize) {
    return fail("invalid entry count");
  }

  for (uint32_t i = 0; i < count; ++i) {
    uint8_t kindValue = 0;
    uint32_t pathLength = 0;
    std::string_view pathBytes;
    if (!reader.readU8(kindValue) || !reader.readU32(pathLength) || pathLength == 0 || pathLength > kMaxPathBytes ||
        !reader.readBytes(pathLength, pathBytes)) {
      return fail("truncated entry");
    }

    const auto kind = static_cast<EntryKind>(kindValue);
    if (kind != EntryKind::File && kind != EntryKind::Directory) {
      return fail("unknown entry kind");
    }

    const QString relative =
        sanitizeRelativePath(QString::fromUtf8(pathBytes.data(), static_cast<qsizetype>(pathBytes.size())));
    if (relative.isEmpty()) {
      return fail("invalid file name");
    }

    const QString absolute = QDir::cleanPath(target + QLatin1Char('/') + relative);
    if (!isInside(absolute, target)) {
      return fail("file name points outside the receive folder");
    }

    const QString top = target + QLatin1Char('/') + relative.section(QLatin1Char('/'), 0, 0);
    const std::string topUtf8 = toUtf8(top);
    if (std::find(topLevel.begin(), topLevel.end(), topUtf8) == topLevel.end()) {
      topLevel.push_back(topUtf8);
      written << top;
    }

    if (kind == EntryKind::Directory) {
      if (!QDir().mkpath(absolute)) {
        return fail("unable to create folder");
      }
      continue;
    }

    uint64_t size = 0;
    std::string_view content;
    if (!reader.readU64(size) || size > reader.remaining() || !reader.readBytes(static_cast<size_t>(size), content)) {
      return fail("truncated file content");
    }

    if (!QDir().mkpath(QFileInfo(absolute).absolutePath())) {
      return fail("unable to create folder");
    }
    QFile file(absolute);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      return fail("unable to create file");
    }
    if (file.write(content.data(), static_cast<qint64>(content.size())) != static_cast<qint64>(content.size())) {
      return fail("unable to write file");
    }
  }

  if (reader.remaining() != 0) {
    return fail("unexpected trailing data");
  }

  return topLevel;
}

std::vector<std::string> ClipboardFiles::receive(const std::string &payload)
{
  std::scoped_lock lock{s_receiveMutex};

  const QByteArray raw = QByteArray::fromRawData(payload.data(), static_cast<qsizetype>(payload.size()));
  const QByteArray hash = QCryptographicHash::hash(raw, QCryptographicHash::Sha256);
  if (hash == s_receivedHash && !s_receivedPaths.empty()) {
    const bool stillThere = std::all_of(s_receivedPaths.begin(), s_receivedPaths.end(), [](const std::string &path) {
      return QFileInfo::exists(fromUtf8(path));
    });
    if (stillThere) {
      LOG_DEBUG("copied files already received, reusing them");
      return s_receivedPaths;
    }
  }

  const QDir receiveDir(fromUtf8(receiveDirectory()));
  if (!QDir().mkpath(receiveDir.absolutePath())) {
    LOG_ERR("unable to create folder for received files: %s", receiveDirectory().c_str());
    return {};
  }
  removeOldTransfers(receiveDir, kKeptTransfers - 1);

  QString name = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
  for (int suffix = 1; receiveDir.exists(name); ++suffix) {
    name = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")) + QStringLiteral("-") +
           QString::number(suffix);
  }

  auto paths = unpack(payload, toUtf8(receiveDir.filePath(name)));
  if (paths.empty()) {
    return {};
  }

  LOG_INFO("received %zu copied items into %s", paths.size(), toUtf8(receiveDir.filePath(name)).c_str());
  s_receivedHash = hash;
  s_receivedPaths = paths;
  return paths;
}

std::string ClipboardFiles::receiveDirectory()
{
  const QString temp = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
  return toUtf8(QDir(temp).filePath(QStringLiteral("Deskflow/ReceivedFiles")));
}

std::string ClipboardFiles::sanitizePathComponent(const std::string &component)
{
  return toUtf8(sanitizeComponent(fromUtf8(component)));
}

bool ClipboardFiles::hasFiles(const IClipboard *clipboard)
{
  if (clipboard == nullptr || !clipboard->open(0)) {
    return false;
  }
  const bool result = clipboard->has(IClipboard::Format::Files);
  clipboard->close();
  return result;
}
