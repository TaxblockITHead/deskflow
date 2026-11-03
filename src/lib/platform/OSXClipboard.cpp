/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2004 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/OSXClipboard.h"

#include "arch/ArchException.h"
#include "base/Log.h"
#include "deskflow/ClipboardFiles.h"
#include "platform/OSXClipboardBMPConverter.h"
#include "platform/OSXClipboardHTMLConverter.h"
#include "platform/OSXClipboardTextConverter.h"
#include "platform/OSXClipboardUTF16Converter.h"
#include "platform/OSXClipboardUTF8Converter.h"

#include <QFileInfo>

#include <climits>

namespace {

// pasteboard flavor of a file copied in Finder, one item per file
const CFStringRef kFileUrlFlavor = CFSTR("public.file-url");

std::string pathFromFileUrlData(CFDataRef data)
{
  std::string result;
  CFURLRef url = CFURLCreateWithBytes(
      kCFAllocatorDefault, CFDataGetBytePtr(data), CFDataGetLength(data), kCFStringEncodingUTF8, nullptr
  );
  if (url == nullptr) {
    return result;
  }

  // Finder uses file reference URLs (file:///.file/id=...), resolve to a path
  CFURLRef pathUrl = CFURLCreateFilePathURL(kCFAllocatorDefault, url, nullptr);
  CFRelease(url);
  if (pathUrl == nullptr) {
    return result;
  }

  UInt8 buffer[PATH_MAX * 4];
  if (CFURLGetFileSystemRepresentation(pathUrl, true, buffer, sizeof(buffer))) {
    result = reinterpret_cast<const char *>(buffer);
  }
  CFRelease(pathUrl);
  return result;
}

} // namespace

//
// OSXClipboard
//

OSXClipboard::OSXClipboard() : m_time(0), m_pboard(nullptr)
{
  m_converters.push_back(new OSXClipboardHTMLConverter);
  m_converters.push_back(new OSXClipboardBMPConverter);
  m_converters.push_back(new OSXClipboardUTF8Converter);
  m_converters.push_back(new OSXClipboardUTF16Converter);
  m_converters.push_back(new OSXClipboardTextConverter);

  OSStatus createErr = PasteboardCreate(kPasteboardClipboard, &m_pboard);
  if (createErr != noErr) {
    LOG_WARN("failed to create clipboard reference: error %i", createErr);
    LOG_ERR("unable to connect to pasteboard, clipboard sharing disabled", createErr);
    m_pboard = nullptr;
    return;
  }

  OSStatus syncErr = PasteboardSynchronize(m_pboard);
  if (syncErr != noErr) {
    LOG_WARN("failed to syncronize clipboard: error %i", syncErr);
  }
}

OSXClipboard::~OSXClipboard()
{
  clearConverters();
}

bool OSXClipboard::empty()
{
  LOG_DEBUG("emptying clipboard");
  if (m_pboard == nullptr)
    return false;

  OSStatus err = PasteboardClear(m_pboard);
  if (err != noErr) {
    LOG_WARN("failed to clear clipboard: error %i", err);
    return false;
  }

  return true;
}

bool OSXClipboard::synchronize()
{
  if (m_pboard == nullptr)
    return false;

  PasteboardSyncFlags flags = PasteboardSynchronize(m_pboard);
  LOG_VERBOSE("flags: %x", flags);

  if (flags & kPasteboardModified) {
    return true;
  }
  return false;
}

void OSXClipboard::add(Format format, const std::string &data)
{
  if (m_pboard == nullptr)
    return;

  LOG_DEBUG("add %d bytes to clipboard format: %d", data.size(), format);
  if (format == IClipboard::Format::Text) {
    LOG_DEBUG("format of data to be added to clipboard was kText");
  } else if (format == IClipboard::Format::Bitmap) {
    LOG_DEBUG("format of data to be added to clipboard was kBitmap");
  } else if (format == IClipboard::Format::HTML) {
    LOG_DEBUG("format of data to be added to clipboard was kHTML");
  } else if (format == IClipboard::Format::Files) {
    LOG_DEBUG("format of data to be added to clipboard was kFiles");
    addFiles(data);
    return;
  }

  for (ConverterList::const_iterator index = m_converters.begin(); index != m_converters.end(); ++index) {

    IOSXClipboardConverter *converter = *index;

    // skip converters for other formats
    if (converter->getFormat() == format) {
      std::string osXData = converter->fromIClipboard(data);
      CFStringRef flavorType = converter->getOSXFormat();
      CFDataRef dataRef = CFDataCreate(kCFAllocatorDefault, (uint8_t *)osXData.data(), osXData.size());
      PasteboardItemID itemID = 0;

      if (dataRef) {
        PasteboardPutItemFlavor(m_pboard, itemID, flavorType, dataRef, kPasteboardFlavorNoFlags);

        CFRelease(dataRef);
        LOG_DEBUG("added %d bytes to clipboard format: %d", data.size(), format);
      }
    }
  }
}

bool OSXClipboard::open(Time time) const
{
  if (m_pboard == nullptr)
    return false;

  LOG_DEBUG("opening clipboard");
  m_time = time;
  return true;
}

void OSXClipboard::close() const
{
  LOG_DEBUG("closing clipboard");
  /* not needed */
}

IClipboard::Time OSXClipboard::getTime() const
{
  return m_time;
}

bool OSXClipboard::has(Format format) const
{
  if (m_pboard == nullptr)
    return false;

  if (format == IClipboard::Format::Files) {
    return hasFiles();
  }

  PasteboardItemID item;
  PasteboardGetItemIdentifier(m_pboard, (CFIndex)1, &item);

  for (ConverterList::const_iterator index = m_converters.begin(); index != m_converters.end(); ++index) {
    IOSXClipboardConverter *converter = *index;
    if (converter->getFormat() == format) {
      PasteboardFlavorFlags flags;
      CFStringRef type = converter->getOSXFormat();

      OSStatus res;

      if ((res = PasteboardGetItemFlavorFlags(m_pboard, item, type, &flags)) == noErr) {
        return true;
      }
    }
  }

  return false;
}

std::string OSXClipboard::get(Format format) const
{
  CFStringRef type;
  PasteboardItemID item;
  std::string result;

  if (m_pboard == nullptr)
    return result;

  if (format == IClipboard::Format::Files) {
    return getFiles();
  }

  PasteboardGetItemIdentifier(m_pboard, (CFIndex)1, &item);

  // find the converter for the first clipboard format we can handle
  IOSXClipboardConverter *converter = nullptr;
  for (ConverterList::const_iterator index = m_converters.begin(); index != m_converters.end(); ++index) {
    converter = *index;

    PasteboardFlavorFlags flags;
    type = converter->getOSXFormat();

    if (converter->getFormat() == format && PasteboardGetItemFlavorFlags(m_pboard, item, type, &flags) == noErr) {
      break;
    }
    converter = nullptr;
  }

  // if no converter then we don't recognize any formats
  if (converter == nullptr) {
    LOG_DEBUG("unable to find converter for data");
    return result;
  }

  // get the clipboard data.
  CFDataRef buffer = nullptr;
  try {
    OSStatus err = PasteboardCopyItemFlavorData(m_pboard, item, type, &buffer);

    if (err != noErr) {
      throw err;
    }

    result = std::string((char *)CFDataGetBytePtr(buffer), CFDataGetLength(buffer));
  } catch (OSStatus err) {
    LOG_DEBUG("exception thrown in OSXClipboard::get MacError (%d)", err);
  } catch (...) {
    LOG_DEBUG("unknown exception in OSXClipboard::get");
    RETHROW_THREADEXCEPTION
  }

  if (buffer != nullptr)
    CFRelease(buffer);

  return converter->toIClipboard(result);
}

bool OSXClipboard::hasFiles() const
{
  PasteboardItemID item;
  if (PasteboardGetItemIdentifier(m_pboard, (CFIndex)1, &item) != noErr) {
    return false;
  }
  PasteboardFlavorFlags flags;
  return PasteboardGetItemFlavorFlags(m_pboard, item, kFileUrlFlavor, &flags) == noErr;
}

std::string OSXClipboard::getFiles() const
{
  ItemCount count = 0;
  if (PasteboardGetItemCount(m_pboard, &count) != noErr) {
    return {};
  }

  std::vector<std::string> paths;
  for (ItemCount index = 1; index <= count; ++index) {
    PasteboardItemID item;
    if (PasteboardGetItemIdentifier(m_pboard, (CFIndex)index, &item) != noErr) {
      continue;
    }
    CFDataRef data = nullptr;
    if (PasteboardCopyItemFlavorData(m_pboard, item, kFileUrlFlavor, &data) != noErr || data == nullptr) {
      continue;
    }
    std::string path = pathFromFileUrlData(data);
    CFRelease(data);
    if (!path.empty()) {
      paths.push_back(path);
    }
  }

  if (paths.empty()) {
    return {};
  }
  LOG_DEBUG("pasteboard has %zu copied files or folders", paths.size());
  return ClipboardFiles::pack(paths);
}

void OSXClipboard::addFiles(const std::string &data)
{
  const auto paths = ClipboardFiles::receive(data);
  for (size_t index = 0; index < paths.size(); ++index) {
    const auto &path = paths[index];
    const Boolean isDirectory = QFileInfo(QString::fromStdString(path)).isDir();
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(path.data()), static_cast<CFIndex>(path.size()),
        isDirectory
    );
    if (url == nullptr) {
      LOG_WARN("unable to create file url for received file: %s", path.c_str());
      continue;
    }

    CFDataRef urlData =
        CFStringCreateExternalRepresentation(kCFAllocatorDefault, CFURLGetString(url), kCFStringEncodingUTF8, 0);
    CFRelease(url);
    if (urlData == nullptr) {
      continue;
    }

    // one item per file, as Finder does, so pasting in Finder copies them all;
    // item 0 is shared with the text added before, like Finder's own items
    auto itemID = reinterpret_cast<PasteboardItemID>(static_cast<uintptr_t>(index));
    PasteboardPutItemFlavor(m_pboard, itemID, kFileUrlFlavor, urlData, kPasteboardFlavorNoFlags);
    CFRelease(urlData);
  }
  LOG_DEBUG("added %zu received files to clipboard", paths.size());
}

void OSXClipboard::clearConverters()
{
  if (m_pboard == nullptr)
    return;

  for (ConverterList::iterator index = m_converters.begin(); index != m_converters.end(); ++index) {
    delete *index;
  }
  m_converters.clear();
}
