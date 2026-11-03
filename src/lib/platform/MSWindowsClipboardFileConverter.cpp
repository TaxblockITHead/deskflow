/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/MSWindowsClipboardFileConverter.h"

#include "base/Log.h"
#include "deskflow/ClipboardFiles.h"

#include <QDir>
#include <QString>

#include <cstring>
#include <shlobj.h>
#include <userenv.h>
#include <wtsapi32.h>

namespace {

// With "Always run as system" Deskflow runs as the SYSTEM account, whose temp
// folder (C:\Windows\SystemTemp) the logged in user can't open, so pasting the
// received files would fail. Use the temp folder of the user logged in to the
// session Deskflow runs in instead. When Deskflow runs as that user,
// WTSQueryUserToken fails and the user's own temp folder is used.
std::string userReceiveDirectory()
{
  DWORD session = 0;
  HANDLE token = nullptr;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &session) || !WTSQueryUserToken(session, &token)) {
    return ClipboardFiles::receiveDirectory();
  }

  std::string result;
  wchar_t profile[MAX_PATH] = {};
  DWORD size = MAX_PATH;
  if (GetUserProfileDirectoryW(token, profile, &size)) {
    const QDir profileDir(QString::fromWCharArray(profile));
    result = profileDir.filePath(QStringLiteral("AppData/Local/Temp/Deskflow/ReceivedFiles")).toStdString();
  }
  CloseHandle(token);

  if (result.empty()) {
    return ClipboardFiles::receiveDirectory();
  }
  LOG_DEBUG("running as another account, receiving files into: %s", result.c_str());
  return result;
}

std::vector<std::string> readDropFiles(const DROPFILES *drop, SIZE_T size)
{
  std::vector<std::string> paths;
  if (size < sizeof(DROPFILES) || drop->pFiles >= size) {
    return paths;
  }

  const char *base = reinterpret_cast<const char *>(drop);
  if (drop->fWide) {
    const auto *cursor = reinterpret_cast<const wchar_t *>(base + drop->pFiles);
    const auto *end = reinterpret_cast<const wchar_t *>(base + size);
    while (cursor < end && *cursor != L'\0') {
      const wchar_t *start = cursor;
      while (cursor < end && *cursor != L'\0') {
        ++cursor;
      }
      paths.push_back(QString::fromWCharArray(start, cursor - start).toStdString());
      ++cursor;
    }
  } else {
    const char *cursor = base + drop->pFiles;
    const char *end = base + size;
    while (cursor < end && *cursor != '\0') {
      const char *start = cursor;
      while (cursor < end && *cursor != '\0') {
        ++cursor;
      }
      paths.push_back(QString::fromLocal8Bit(start, cursor - start).toStdString());
      ++cursor;
    }
  }
  return paths;
}

} // namespace

IClipboard::Format MSWindowsClipboardFileConverter::getFormat() const
{
  return IClipboard::Format::Files;
}

UINT MSWindowsClipboardFileConverter::getWin32Format() const
{
  return CF_HDROP;
}

HANDLE MSWindowsClipboardFileConverter::fromIClipboard(const std::string &data) const
{
  const auto paths = ClipboardFiles::receive(data, userReceiveDirectory());
  if (paths.empty()) {
    return nullptr;
  }

  // DROPFILES header followed by a double null terminated list of paths
  std::wstring list;
  for (const auto &path : paths) {
    list += QDir::toNativeSeparators(QString::fromStdString(path)).toStdWString();
    list += L'\0';
  }
  list += L'\0';

  const SIZE_T listBytes = list.size() * sizeof(wchar_t);
  HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + listBytes);
  if (handle == nullptr) {
    LOG_ERR("unable to allocate memory for received files list");
    return nullptr;
  }

  auto *drop = static_cast<DROPFILES *>(GlobalLock(handle));
  if (drop == nullptr) {
    GlobalFree(handle);
    return nullptr;
  }
  drop->pFiles = sizeof(DROPFILES);
  drop->fWide = TRUE;
  std::memcpy(reinterpret_cast<char *>(drop) + sizeof(DROPFILES), list.data(), listBytes);
  GlobalUnlock(handle);

  return handle;
}

std::string MSWindowsClipboardFileConverter::toIClipboard(HANDLE data) const
{
  const SIZE_T size = GlobalSize(data);
  const auto *drop = static_cast<const DROPFILES *>(GlobalLock(data));
  if (drop == nullptr) {
    return {};
  }
  const auto paths = readDropFiles(drop, size);
  GlobalUnlock(data);

  if (paths.empty()) {
    return {};
  }
  LOG_DEBUG("clipboard has %zu copied files or folders", paths.size());
  return ClipboardFiles::pack(paths);
}
