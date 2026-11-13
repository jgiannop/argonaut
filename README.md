# Kaftilous

A fast file manager built with C++ and Qt 6, inspired by GNOME Files (Nautilus).

> **Status:** early development, used daily on Ubuntu 24.04. The code is written to be cross-platform (Linux, Windows, macOS), but only Linux has been tested so far.

## Features

**Browsing**
- Grid view with thumbnails, and a list view with sortable **Name / Size / Type / Modified** columns
- Thumbnails for photos, **PDFs, videos, fonts, comics and more**, using the thumbnailers already installed on your system (the same ones Nautilus uses) and sharing their cache. They run **sandboxed** (no network, nothing visible but the one file), two at a time at low priority, only for files on screen, and by default only on the computer's own disks (Preferences)
- Folders first, in natural order ("file2" before "file10"); sort by name, size, type or date
- Sidebar with your folders, drives and the Trash; a breadcrumb path bar (Ctrl+L to type a path)
- **Recent**: what you opened lately, in Kaftilous or any other app (the list GNOME apps share), newest first. Respects GNOME's "remember recent files" privacy switch. Right-click → Remove from Recent
- **Starred**: star files and folders (right-click → Star); they get a ★ and their own place in the sidebar. Stars follow items you rename or move
- Search (Ctrl+F) in the folder **and all its sub-folders**, with results as you type. Several words narrow it down, and case and accents don't matter ("θαλασσα" finds "Θάλασσα.jpg", "cafe" finds "Café"). Right-click → Open Item Location
- Show hidden files (Ctrl+H)
- **Quick Look** (Space): a preview of the selected file without opening an app: photos (animated GIFs play), PDFs, videos, fonts, text and code, the contents of archives (nothing is extracted), folders. Arrow keys go to the next file, Enter opens it, Space or Esc closes. Programs disguised as documents are never previewed. Previews are made in the same sandbox as thumbnails
- **Split view** (F3): two folders side by side, each with its own history and view. F5 copies the selection to the other side, F6 moves it, or drag between them. Delete, rename and every other shortcut act on the side you're in
- Zoom (Ctrl+plus / Ctrl+minus / Ctrl+0, or Ctrl+mouse wheel): grid icons from 32 to 256 px, list icons from 16 to 48 px, remembered separately for each
- **Analyze view** (Ctrl+3, or the pie button): how much disk space every folder and sub-folder uses, as a tree with usage bars, biggest first. Sizes match `du` exactly (space on disk, hard links counted once; symlinks and other drives aren't followed). Folders appear while it measures; opening a folder inside what's been measured is instant. **Show Files When Analyzing** (View menu or right-click) also lists each folder's biggest files. Copying, moving, trashing, extracting and Undo update the sizes at once, without measuring everything again. Right-click to open, trash or scan again
- Live updates when files change on disk
- Fast with big folders: 20,000 files load in about 0.2 s (Nautilus needs several times longer just to gather its file information)

**Files**
- Copy, cut and paste, which also work with other file managers' clipboards
- Drag and drop: same drive moves, another drive copies, Ctrl copies, Shift moves
- Progress for long operations, with time left and cancel
- Undo (Ctrl+Z) and Redo (Ctrl+Shift+Z or Ctrl+Y) for copy, move, trash, rename, new items, extract and compress. Redo brings back the very same items (for example, from the Trash), it never copies again
- Never overwrites: name clashes become "name (copy)"
- New Folder, New File, Rename (F2)
- USB sticks: Kaftilous says "done" only after the data is really written to the drive

**Archives** (with libarchive)
- Right-click → **Extract Here** / **Extract to…** for zip, 7z, tar.gz/xz/bz2/zst, rar, iso, deb, plain .gz/.xz and more
- Smart like Nautilus: one item inside comes out as is; several go into a folder named after the archive
- Safe: never overwrites, and entries that try to escape the folder (`../`, absolute paths, symlink tricks) are skipped and reported
- Uses the real type: a `.zip` that is really 7z still extracts; a program named `.zip` is never treated as an archive
- **Compress…** any selection to .zip, .tar.xz, .tar.gz or .7z
- Progress, cancel (nothing half-finished is left behind) and Undo, like copying

**Real file types: safer than trusting names**

Most file managers, Nautilus included, believe a file's extension: `report.pdf` is shown and opened as a PDF without anyone looking inside. Kaftilous checks what each file really is, from its first few kilobytes, and tells you when the name lies:
- An **amber ⚠ badge** marks files whose content doesn't match their name, for example a `.pdf` that is really an HTML error page from a failed download, or a `.zip` that is really a 7z archive. Hover for details. Right-click → **Rename to “report.html”** fixes it (and can be undone).
- A **red ⚠ badge** marks **programs disguised as documents** (`invoice.pdf` that is really an executable). Opening one shows a warning; Kaftilous never runs it.
- Files are **opened with the app for what they really are**, so a mislabelled file still opens correctly.
- Files with no extension get their real type and icon.
- **Properties** shows both what the name says and what the file is.
- **It doesn't slow anything down.** The folder appears instantly from the names, the check runs in the background, and the results are remembered (in `~/.cache`), so each file is read only once until it changes. On USB sticks and network drives only the files on screen are checked.

**Trash** (Linux)
- Restore to the original location, Delete Permanently (Shift+Delete), Empty Trash
- Shows items trashed on all drives, including by other apps

**Extras**
- Embedded terminal (Ctrl+`) that opens in the current folder
- Themes: Follow System, Light, Dark, Midnight Blue, Rosé Dawn, Forest, Sunset (Preferences, Ctrl+,)
- Tabs (Ctrl+T), bookmarks shared with Nautilus (Ctrl+D), eject for USB drives, Open With…, Properties (Ctrl+I), type-to-find
- Sizes in GNOME's units (kB, MB, GB), matching Nautilus and drive labels

### Keyboard shortcuts

| Action | Shortcut |
|---|---|
| Back / Forward / Up | Alt+← / Alt+→ / Alt+↑ |
| Home folder | Alt+Home |
| Type a path | Ctrl+L |
| Search (this folder and sub-folders) | Ctrl+F |
| Grid / list / analyze disk usage | Ctrl+1 / Ctrl+2 / Ctrl+3 |
| Quick Look (preview) | Space |
| Split view / copy to other side / move to other side | F3 / F5 / F6 |
| Show hidden files | Ctrl+H |
| Copy / Cut / Paste | Ctrl+C / Ctrl+X / Ctrl+V |
| Move to Trash / Delete permanently | Delete / Shift+Delete |
| Undo / Redo | Ctrl+Z / Ctrl+Shift+Z or Ctrl+Y |
| Zoom in / out / normal size | Ctrl++ / Ctrl+− / Ctrl+0 (or Ctrl+wheel) |
| Rename | F2 |
| New folder / New file | Ctrl+Shift+N / Ctrl+Alt+N |
| New tab / Close tab / Reopen closed tab | Ctrl+T / Ctrl+W / Ctrl+Shift+T |
| Next / previous tab | Ctrl+Tab / Ctrl+Shift+Tab |
| Bookmark this folder | Ctrl+D |
| Properties | Ctrl+I or Alt+Enter |
| Find a file by typing | just start typing in the folder |
| Terminal | Ctrl+` |
| Preferences | Ctrl+, |

## Building

### Requirements

- A C++17 compiler (GCC, Clang or MSVC)
- CMake 3.21 or newer, and Ninja
- Qt 6.4 or newer (Widgets; Test for the tests)
- Optional: **libvterm** for the embedded terminal (Linux/macOS). Without it, Kaftilous builds without the terminal.
- Optional: **libarchive** for Compress / Extract. Without it, those menu items are hidden.

### Linux (Ubuntu / Debian)

```bash
sudo apt install build-essential cmake ninja-build qt6-base-dev qt6-tools-dev qt6-wayland libvterm-dev libarchive-dev
```

### Windows and macOS

Install Qt 6 with the [Qt Online Installer](https://www.qt.io/download-qt-installer), plus CMake and Ninja. If CMake can't find Qt, point it at your installation with `-DCMAKE_PREFIX_PATH=<path-to-Qt>/6.x.x/<compiler>`.

### Build, run, test

```bash
cmake --preset debug
cmake --build build/debug
./build/debug/kaftilous [folder]

ctest --preset debug      # run the tests
```

For an optimized build, use `release` instead of `debug`.

## Installing (Linux)

Kaftilous installs for your user only, in `~/.local`: no `sudo`, nothing outside your home folder.

```bash
cmake --preset release
cmake --build build/release
cmake --install build/release      # ~/.local/bin/kaftilous, menu entry, icon
```

It then appears in the app grid as **Kaftilous**. To uninstall: `cmake --build build/release --target uninstall`.

### Using it instead of Nautilus

Open **Preferences → Default File Manager → Make Kaftilous the Default**, or run `kaftilous --make-default`. Kaftilous then opens:
- folders opened from other apps, desktop icons, Super+E and "Open containing folder" (it becomes the default app for folders);
- "Show in folder" from Firefox, Chrome and others (it answers `org.freedesktop.FileManager1` on D-Bus).

To go back: **Preferences → Use Nautilus Again**, or `kaftilous --restore-nautilus`, which works from any terminal even if Kaftilous itself won't start. Nautilus is never removed or changed; switching only points these two hooks at one or the other, and the previous file manager is remembered.

Notes:
- If Nautilus is already running in the background, "Show in folder" keeps going to it until it stops (`nautilus -q`, or log out and in once).
- The dock's **Files** icon still starts Nautilus: right-click it → *Unpin*, then right-click Kaftilous → *Pin to Dash*.

## Developing in VS Code

Open the folder in VS Code and install the recommended extensions when asked (C/C++, CMake Tools, Qt). Pick the **Debug** preset, then press **F7** to build or **F5** to run with the debugger. Tests appear in the Testing panel.

## Project layout

```
src/               Application source code
tests/             Qt Test suites (run with ctest)
CMakeLists.txt     Build definition
CMakePresets.json  Debug and release build and test presets
.vscode/           VS Code build, debug and formatting settings
CLAUDE.md          Architecture notes for contributors
```
