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
- The look copies GNOME Files (Nautilus) / libadwaita, but it's built only from Qt, so it looks the same on every platform:
  - `Theme` (`src/Theme.{h,cpp}`) holds the light and dark colour tokens, chosen from `QApplication::palette()`. Use the application palette, not the widget palette: our own style sheet changes widget palettes.
  - `glyphIcon(Glyph, color)` draws every UI icon in code through a custom `QIconEngine`. Don't depend on the system icon theme or the Qt SVG plugin for UI icons. File-type icons still come from `QFileSystemModel` / the OS.
  - `MainWindow::applyTheme()` builds one window-wide style sheet (`styleSheetFor`, using `@token` placeholders) and re-creates every themed icon. It runs again on `ApplicationPaletteChange`. Anything with a themed icon must be updated there.
- Window chrome: on Linux and Windows the window is frameless (client-side decorations). Set `KAFTILOUS_NATIVE_DECORATIONS=1` to turn that off; macOS always uses the native frame.
  - Two `HeaderBar`s, one above the sidebar and one above the content, act as the title bar. Dragging empty space calls `startSystemMove`; double-click maximises.
  - Edge resizing is handled by an event filter on the `QWindow` (`MainWindow::eventFilter` → `startSystemResize`).
  - The window buttons only exist when `m_csd` is true.
- Main view:
  - There is one `QFileSystemModel` with `setRootPath("")`, shared by a single `QListView`. Changing folder only calls `setRootIndex`. Folder loading is asynchronous: anything that needs a folder's contents must wait for `directoryLoaded`.
  - Grid/list is the same view switched between `IconMode` and `ListMode` (`setGridMode`). Grid cell size comes from `FileItemDelegate::gridCellSize`.
  - `FileItemDelegate` paints every item itself: wrapped, middle-elided names, rounded hover/selection, list-mode size/date.
  - `Thumbnailer` loads thumbnails in a thread pool. It reads the freedesktop cache (`~/.cache/thumbnails`) on Linux first, then decodes images with `QImageReader`. It emits `thumbnailReady`, which repaints the viewport.
  - Search (Ctrl+F) only filters the current folder through the model's name filters (`updateFilter`). It drops `QDir::AllDirs` while active so folders are filtered too.
- `navigateTo(path, addToHistory)` is the **only** way to change the current folder. It validates and cleans the path into `m_current` (always with `/` separators; the native form is only for display), leaves search mode, and updates history, `PathBar`, the title, the sidebar selection and the floating bar. Everything that changes folder goes through it: actions, sidebar, breadcrumbs, the typed path, double-click and the command-line argument.
- `PathBar` shows breadcrumbs. Leading crumbs are hidden when space is short, and it switches to a `QLineEdit` on click or Ctrl+L. It emits `pathActivated`.
- Sidebar entries are `QListWidgetItem`s with the path in `PathRole` (`Qt::UserRole`). Separator rows have `SeparatorRole` and an empty path.
- There is no status bar. `m_floatingBar` (a `QLabel` over the view) shows selection info and temporary messages (`showMessage`).
