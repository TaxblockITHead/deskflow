/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/IClipboard.h"

#include <cstdint>
#include <string>
#include <vector>

//! Copy and paste of files through the shared clipboard
/*!
When files are copied in Explorer or Finder, the platform clipboard
packs them (including whole folders) into an IClipboard::Format::Files
payload with pack(). The peer extracts the payload with receive() and
puts references to the extracted files on its own clipboard, so that
pasting in Explorer or Finder copies them to the destination.

Payload layout (all integers are big endian):
\code
  4 bytes  magic "DFF1"
  4 bytes  number of entries
  per entry:
    1 byte   kind (0 = file, 1 = directory)
    4 bytes  path length n
    n bytes  relative path, UTF-8, '/' separated; the first component is
             the name of the item that was copied
    files only:
      8 bytes  content length m
      m bytes  content
\endcode
*/
class ClipboardFiles
{
public:
  //! Upper bound for a payload, whatever the configured clipboard limit
  /*!
  The whole payload is held in memory (several times over while it is
  being sent), and clipboard marshalling uses 32-bit sizes.
  */
  static constexpr uint64_t kHardLimitBytes = 512ull * 1024 * 1024;

  //! Number of received transfers kept in the receive directory
  static constexpr int kKeptTransfers = 3;

  //! Set the largest payload that pack() will build
  /*!
  Called with the configured clipboard size limit so that files which
  would be rejected anyway are not read from disk. Values above
  kHardLimitBytes are clamped.
  */
  static void setMaxPayloadBytes(uint64_t bytes);

  //! Get the largest payload that pack() will build
  static uint64_t maxPayloadBytes();

  //! Pack files and directories into a Files payload
  /*!
  \p paths are absolute UTF-8 paths of the copied items. Directories are
  included recursively; symbolic links inside them are skipped. Returns
  an empty string if there is nothing to send or if the total size is
  over maxPayloadBytes(). The last result is cached and reused while
  the files are unchanged, because the clipboard is read again every
  time the cursor leaves the computer.
  */
  static std::string pack(const std::vector<std::string> &paths);

  //! Pack with an explicit size limit and without the cache
  static std::string pack(const std::vector<std::string> &paths, uint64_t maxBytes);

  //! Extract a Files payload into \p targetDir
  /*!
  Entry paths are sanitized so that nothing is written outside
  \p targetDir. Returns the absolute paths ('/' separated) of the
  top-level items, or an empty list if the payload is invalid (in which
  case anything already extracted is removed).
  */
  static std::vector<std::string> unpack(const std::string &payload, const std::string &targetDir);

  //! Extract a Files payload into a new folder in receiveDirectory()
  /*!
  Receiving the same payload twice in a row reuses the earlier folder.
  Older transfers beyond kKeptTransfers are deleted.
  */
  static std::vector<std::string> receive(const std::string &payload);

  //! Folder holding the files received from other computers
  static std::string receiveDirectory();

  //! Make a path component safe to create on Windows, macOS and Linux
  /*!
  Returns an empty string for components that must be rejected
  (empty, "." and "..").
  */
  static std::string sanitizePathComponent(const std::string &component);

  //! True if \p clipboard contains copied files
  static bool hasFiles(const IClipboard *clipboard);
};

//! Read-only view of a clipboard that hides the Files format
/*!
Windows and macOS have a single clipboard, which Deskflow reports as
both kClipboardClipboard and kClipboardSelection. Copied files are sent
with kClipboardClipboard only; this view is used for kClipboardSelection
so the files are not read and sent a second time.
*/
class ClipboardWithoutFiles : public IClipboard
{
public:
  explicit ClipboardWithoutFiles(const IClipboard &source) : m_source(source)
  {
    // do nothing
  }

  bool empty() override
  {
    return false;
  }
  void add(Format, const std::string &) override
  {
    // read-only
  }
  bool open(Time time) const override
  {
    return m_source.open(time);
  }
  void close() const override
  {
    m_source.close();
  }
  Time getTime() const override
  {
    return m_source.getTime();
  }
  bool has(Format format) const override
  {
    return format != Format::Files && m_source.has(format);
  }
  std::string get(Format format) const override
  {
    return format == Format::Files ? std::string() : m_source.get(format);
  }

private:
  const IClipboard &m_source;
};
