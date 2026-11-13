# Kaftilous

A fast, simple, cross-platform file manager for Linux, Windows and macOS, built with C++ and Qt 6.

> **Status:** early development. Expect missing features and rough edges.

## Features

- Sidebar with your common folders (Home, Desktop, Documents, Downloads, Music, Pictures, Videos) and mounted drives
- Back, Forward, Up and Home navigation
- Editable path bar
- Grid and list views
- Show or hide hidden files
- Opens files in their default application
- Can start in any folder: `kaftilous ~/Downloads`

### Keyboard shortcuts

| Action | Shortcut |
|---|---|
| Back / Forward | Alt+← / Alt+→ |
| Up one folder | Alt+↑ |
| Home folder | Alt+Home |
| Edit path | Ctrl+L |
| Show hidden files | Ctrl+H |
| Open selected item | Enter or double-click |

## Building

### Requirements

- A C++17 compiler (GCC, Clang or MSVC)
- CMake 3.21 or newer, and Ninja
- Qt 6.4 or newer (Widgets module)

### Linux (Ubuntu / Debian)

```bash
sudo apt install build-essential cmake ninja-build qt6-base-dev qt6-tools-dev designer-qt6 qt6-wayland
```

### Windows and macOS

Install Qt 6 with the [Qt Online Installer](https://www.qt.io/download-qt-installer), plus CMake and Ninja. If CMake can't find Qt, point it at your installation with `-DCMAKE_PREFIX_PATH=<path-to-Qt>/6.x.x/<compiler>`.

### Build and run

```bash
cmake --preset debug
cmake --build build/debug
./build/debug/kaftilous
```

For an optimized build, use `release` instead of `debug`.

## Developing in VS Code

Open the folder in VS Code and install the recommended extensions when asked: C/C++, CMake Tools and Qt. Pick the **Debug** preset, then press **F7** to build or **F5** to run with the debugger.

## Project layout

```
src/            Application source code
CMakeLists.txt  Build definition
CMakePresets.json  Debug and release build presets
.vscode/        VS Code build and debug settings
```
