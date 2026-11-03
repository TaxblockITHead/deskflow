# Copying files between computers

This fork of Deskflow can copy **files and folders** (PDF, Excel, zip, PowerPoint, whole folders…) between computers, in addition to text. It works between Windows and macOS (Intel and Apple Silicon), in both directions.

You copy and paste exactly as you would on one computer:

1. Select files or folders in **Explorer** or **Finder** and copy them (`Ctrl+C` / `Cmd+C`).
2. Move the mouse to the other computer.
3. Open an Explorer or Finder window and paste (`Ctrl+V` / `Cmd+V`).

## 1. Which installer each computer needs

All computers in the setup must run this build. A computer running the official Deskflow ignores copied files (text still works).

| Computer | Installer | Needs |
|---|---|---|
| Windows PC (e.g. the Dell) | `package-deskflow-windows-2022-x64` | Windows 10 or 11, 64-bit |
| Intel Mac (e.g. MacBook Pro 2017) | `package-deskflow-macos-x64` | macOS 12 or newer |
| Apple Silicon Mac (M1–M4, e.g. MacBook Air M4) | `package-deskflow-macos-arm64` | macOS 14 or newer |

### Where to download

The installers are built by GitHub Actions. The latest ones are on this run (sign in to GitHub, scroll to **Artifacts**):

https://github.com/TaxblockITHead/deskflow/actions/runs/41598513089

Each artifact is a zip that contains the installer. These artifacts expire on **1 February 2027**. To get fresh ones, see [Building new installers](#7-building-new-installers).

## 2. Installing

Quit Deskflow on the computer first.

### Windows

1. Unzip the artifact and run the installer.
2. Open Deskflow → **Settings** and **untick "Always run as system (work at login screen and UAC)"**, then click OK. See [Known limitations](#5-known-limitations) for why.

### macOS (Intel and Apple Silicon)

The Mac builds are not signed with an Apple Developer certificate, so macOS warns that the app may be malicious. This is expected.

1. Open the `.dmg` and drag **Deskflow** into **Applications**. If a warning appears, click **Done** (not "Move to Trash").
2. Open **Terminal** and run:

   ```bash
   sudo xattr -dr com.apple.quarantine /Applications/Deskflow.app
   sudo codesign --force --deep --sign - /Applications/Deskflow.app
   ```

   The first command removes the "downloaded from the internet" flag. The second signs the app locally, which Apple Silicon Macs require.
3. Open Deskflow and allow **Accessibility** when asked. If you had Deskflow installed before, reset its old permission first:

   ```bash
   pkill -f Deskflow; pkill -f deskflow-core
   tccutil reset Accessibility org.deskflow.deskflow
   tccutil reset ListenEvent org.deskflow.deskflow
   tccutil reset PostEvent org.deskflow.deskflow
   open /Applications/Deskflow.app
   ```

   Turn the permission on in **System Settings → Privacy & Security → Accessibility**, then **quit and reopen Deskflow**. The permission only takes effect after a restart.
4. When Deskflow first sends a file from Desktop, Documents or Downloads, macOS may ask for access to that folder. Click **Allow**.

## 3. Settings

### Clipboard size limit (server only)

The copied files count toward the clipboard size limit, which is **3 MB by default**. Raise it on the computer that runs the Deskflow **server**; clients use the server's value.

1. Deskflow → **Configure Server** → **Advanced** tab.
2. Tick **Enable clipboard sharing**.
3. Set **Limit to** to about **150 MB**. Don't go much higher on Macs with 8 GB of memory: a transfer is held in memory several times while it is sent. The hard maximum is 512 MB.
4. Click **OK** and restart the server.

Files over the limit are not sent. The log then says `not sharing copied files, total size … is over the clipboard size limit`.

### "Always run as system" (Windows only)

Keep it **off** on Windows computers that need to **send** files. See [Known limitations](#5-known-limitations).

## 4. Where received files go

The receiving computer stores the files in a temporary folder until you paste them. Pasting copies them to where you paste.

| System | Folder |
|---|---|
| Windows | `%LOCALAPPDATA%\Temp\Deskflow\ReceivedFiles\` |
| macOS | `$TMPDIR/Deskflow/ReceivedFiles/` |

Each copy goes into its own sub-folder named by date and time. Only the last 3 transfers are kept; older ones are deleted automatically.

## 5. Known limitations

- **Windows with "Always run as system" on:** files copied on that Windows computer are **not sent**. Receiving files still works. In this mode Deskflow runs as the Windows SYSTEM account, and Windows doesn't give the list of files copied in Explorer to that account. Turn the option off to send files. The cost is that Deskflow can't control the Windows login screen or UAC prompts.
- **Large files make the mouse lag.** Files are sent when the mouse moves to the other computer, over the same connection as the mouse and keyboard. Expect a few seconds of lag per 100 MB over Wi-Fi. Use cable or 5 GHz Wi-Fi for large files.
- **Copy, not move.** Cut (`Ctrl+X`) on Windows behaves like copy: the original file stays where it is.
- **Not copied:** symbolic links inside folders, and Finder's `.DS_Store` files.
- **File names** with characters Windows doesn't allow (`< > : " | ? *`) arrive with `_` in their place.
- **Linux** computers don't send or receive files yet; they keep sharing text.

## 6. Troubleshooting

| Problem | What to check |
|---|---|
| Paste does nothing | Both computers run this build? Clipboard limit raised on the server and the server restarted? |
| Nothing is sent from Windows | "Always run as system" must be off. |
| Mac won't open Deskflow | Run the two Terminal commands in [Installing](#macos-intel-and-apple-silicon). |
| Mac asks for Accessibility again and again | Reset the permission as shown in step 3 of [Installing](#macos-intel-and-apple-silicon), then quit and reopen Deskflow. |
| Big file doesn't arrive | It's over the clipboard limit; look for `not sharing copied files` in the log. |

### Reading the log

Deskflow → **Settings** → **Log level → Debug** gives the most detail. Useful lines:

| Line | Meaning |
|---|---|
| `sharing N copied file entries, X KB` | This computer packed the copied files and will send them. |
| `received N copied items into …` | This computer received files; this is the folder they are in. |
| `not sharing copied files, …` | Files were not sent, and why. |
| `unable to receive copied files: …` | Received data was invalid and was discarded. |

On Windows, when Deskflow runs as a service, the log is in `C:\ProgramData\Deskflow\deskflow-daemon.log`.

## 7. Building new installers

### With GitHub Actions (recommended)

The GitHub workflow builds the installers for every pull request and every push to `master`.

1. Push your branch to `TaxblockITHead/deskflow`.
2. Open a pull request **within the fork** (base `TaxblockITHead/deskflow` `master`, not the original Deskflow project).
3. Wait for the **Continuous Integration** run (about 40 minutes). Download the installers from the run's **Artifacts** section.

A commit whose message contains `[skip ci]` does not start the workflow.

### On your own computer

Follow [docs/dev/build.md](dev/build.md). In short:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build/src/unittests --output-on-failure
cmake --build build --target package   # installer / dmg
```

Windows needs Visual Studio 2022, Qt 6.7+ and vcpkg. macOS needs Xcode and Qt 6.7+. Linux needs the packages listed in `.github/actions/install-dependencies/action.yml`.

## 8. For developers: what was changed

The feature adds a clipboard format, `IClipboard::Format::Files`, that carries the copied files. It is sent through the existing chunked clipboard messages (`DCLP`), so the network protocol did not change. Peers that don't know the format skip it.

| File | What it does |
|---|---|
| `src/lib/deskflow/IClipboard.h` | Adds `Format::Files`. |
| `src/lib/deskflow/ClipboardFiles.h/.cpp` | Packs files and folders into one payload and extracts it. Applies the size limit before reading from disk, cleans file names, rejects paths that point outside the receive folder, reuses identical payloads, deletes old transfers. Also has `ClipboardWithoutFiles`, a view of a clipboard without the files. |
| `src/lib/platform/MSWindowsClipboardFileConverter.h/.cpp` | Windows: reads Explorer's `CF_HDROP` file list and writes received files back as `CF_HDROP`. Receives into the logged in user's temp folder, also when Deskflow runs as SYSTEM. |
| `src/lib/platform/MSWindowsClipboard.cpp` | Registers the Windows converter. |
| `src/lib/platform/MSWindowsComputer.cpp` | Sends files with clipboard 0 only (Windows has one clipboard for both Deskflow clipboard ids), and keeps received files from being replaced by the duplicate selection clipboard. |
| `src/lib/platform/OSXClipboard.h/.cpp` | macOS: reads Finder's `public.file-url` pasteboard items (including Finder's `file:///.file/id=` reference URLs) and writes received files as one pasteboard item per file. |
| `src/lib/platform/OSXComputer.mm` | Same clipboard 0 / selection handling as on Windows. |
| `src/lib/server/Server.cpp`, `src/lib/client/Client.cpp` | Pass the configured clipboard size limit to `ClipboardFiles`, so files that are too large are never read. |
| `src/lib/deskflow/Clipboard.h`, `src/lib/platform/EiClipboard.h` | Array initialisers sized for the new format. |
| `src/lib/deskflow/CMakeLists.txt`, `src/lib/platform/CMakeLists.txt` | Build the new files; link `platform` with `app` on Windows. |
| `src/unittests/deskflow/ClipboardFilesTests.h/.cpp` | Unit tests: round trip of files and folders, size limit, path traversal, truncated and unknown data, Windows name cleaning, duplicate payloads, clipboard marshalling. |
| `docs/Configuration.md`, `docs/FileCopy.md` | User documentation. |

### Payload format

All integers are big endian.

```
4 bytes   magic "DFF1"
4 bytes   number of entries
per entry:
  1 byte    kind (0 = file, 1 = folder)
  4 bytes   path length n
  n bytes   relative path, UTF-8, '/' separated, first part is the copied item's name
  files only:
    8 bytes   content length m
    m bytes   content
```
