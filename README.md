# Kaftilous

A fast file manager built with C++ and Qt 6, inspired by GNOME Files (Nautilus).

> **Status:** early development, used daily on Ubuntu 24.04. The code is written to be cross-platform (Linux, Windows, macOS), but only Linux has been tested so far.

## Features

**Browsing**
- Grid view with thumbnails, and a list view with sortable **Name / Size / Type / Modified** columns
- Folders first, in natural order ("file2" before "file10"); sort by name, size, type or date
- Sidebar with your folders, drives and the Trash; a breadcrumb path bar (Ctrl+L to type a path)
- Search the current folder (Ctrl+F), show hidden files (Ctrl+H)
- Live updates when files change on disk
- Fast with big folders: 20,000 files load in about 0.2 s (Nautilus needs several times longer just to gather its file information)

**Files**
- Copy, cut and paste, which also work with other file managers' clipboards
- Drag and drop: same drive moves, another drive copies, Ctrl copies, Shift moves
- Progress for long operations, with time left and cancel
- Undo (Ctrl+Z) for copy, move, trash, rename, new items, extract and compress
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
| Search this folder | Ctrl+F |
| Grid / list view | Ctrl+1 / Ctrl+2 |
| Show hidden files | Ctrl+H |
| Copy / Cut / Paste | Ctrl+C / Ctrl+X / Ctrl+V |
| Move to Trash / Delete permanently | Delete / Shift+Delete |
| Undo | Ctrl+Z |
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
