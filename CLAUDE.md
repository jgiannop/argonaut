# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Kaftilous is a cross-platform (Linux, Windows, macOS) desktop file manager written in C++17 with Qt 6 Widgets. It has to keep working on all three platforms, so use Qt APIs (`QStorageInfo`, `QStandardPaths`, `QDesktopServices`, `QFile::moveToTrash`, etc.) instead of OS-specific calls. Where OS-specific behaviour is unavoidable, put it behind `Q_OS_LINUX` / `Q_OS_WIN` / `Q_OS_MACOS` guards, as `MainWindow::populateSidebar()` does.

The minimum Qt version is **6.4** (what Ubuntu 24.04 ships). Don't use APIs added after 6.4 unless you also raise `find_package(Qt6 6.4 ...)` in `CMakeLists.txt`.

## Build and run

CMake presets (`CMakePresets.json`) use the Ninja generator and build into `build/<preset>`:

```bash
cmake --preset debug            # configure (or: --preset release)
cmake --build build/debug       # build
./build/debug/kaftilous [path]  # run; optional start folder (path or file:// URL)
```

In VS Code, CMake Tools is set to use the presets. F7 builds and F5 debugs with gdb (`.vscode/launch.json` runs the `CMake: build` task first).

Linux dependencies: `cmake ninja-build qt6-base-dev qt6-tools-dev designer-qt6 qt6-wayland`.

There are no tests and no linter yet.

## Architecture

- `qt_standard_project_setup()` turns on AUTOMOC/AUTOUIC. Every new source or header must be listed in `qt_add_executable(...)` in `CMakeLists.txt`, or moc won't run on it.
- `MainWindow` (`src/MainWindow.{h,cpp}`) holds the whole UI for now:
  - It has one `QFileSystemModel` with `setRootPath("")`, shared by the `QListView`. Changing folder never resets the model; it only calls `m_view->setRootIndex(m_model->index(path))`. Folder loading is asynchronous: anything that needs the contents of a folder (for example the item count) must wait for `QFileSystemModel::directoryLoaded`.
  - `navigateTo(path, addToHistory)` is the **only** way to change the current folder. It validates the path, cleans it into `m_current` (always with `/` separators; the native form is only for display), updates the back/forward stacks, the path bar, the window title and the action states. The back/forward/up/home actions, the sidebar, the path bar, double-click and the command-line argument in `main.cpp` all go through it.
  - Grid/list is one `QListView` switching between `IconMode` and `ListMode` (`setGridMode`). The hidden-files toggle changes the model's `QDir` filter.
  - Sidebar entries are `QListWidgetItem`s with the target path stored in `Qt::UserRole`. An item with an empty path, like the separator, is ignored on click.
