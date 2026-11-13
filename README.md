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
- Fast with big folders: 20,000 files load in about 0.2 s

**Files**
- Copy, cut and paste, which also work with other file managers' clipboards
- Drag and drop: same drive moves, another drive copies, Ctrl copies, Shift moves
- Progress for long operations, with time left and cancel
- Undo (Ctrl+Z) for copy, move, trash, rename and new items
- Never overwrites: name clashes become "name (copy)"
- New Folder, New File, Rename (F2)
- USB sticks: Kaftilous says "done" only after the data is really written to the drive

**Trash** (Linux)
- Restore to the original location, Delete Permanently (Shift+Delete), Empty Trash
- Shows items trashed on all drives, including by other apps

**Extras**
- Embedded terminal (Ctrl+`) that opens in the current folder
- Themes: Follow System, Light, Dark, Midnight Blue, Rosé Dawn, Forest, Sunset (Preferences, Ctrl+,)

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
| Terminal | Ctrl+` |
| Preferences | Ctrl+, |

## Building

### Requirements

- A C++17 compiler (GCC, Clang or MSVC)
- CMake 3.21 or newer, and Ninja
- Qt 6.4 or newer (Widgets; Test for the tests)
- Optional: **libvterm** for the embedded terminal (Linux/macOS). Without it, Kaftilous builds without the terminal.

### Linux (Ubuntu / Debian)

```bash
sudo apt install build-essential cmake ninja-build qt6-base-dev qt6-tools-dev qt6-wayland libvterm-dev
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
