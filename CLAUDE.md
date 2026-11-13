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

Tests (Qt Test + CTest, in `tests/`; they run headless and sandbox `HOME`/the trash):

```bash
ctest --preset debug                                    # all tests
./build/debug/tests/tst_fileoperations copyFolderTree   # one test function
```

In VS Code, CMake Tools' Testing panel runs them too. Formatting: `.clang-format` (Qt-like). Format-on-save applies to changed lines only. There is no linter.

## Architecture

- `qt_standard_project_setup()` turns on AUTOMOC/AUTOUIC. Every new source or header must be listed in `CMakeLists.txt`, or moc won't run on it. The GUI goes in `qt_add_executable(kaftilous ...)`. The file-operations engine (`FileJob`, `FileOperations`) is the static library `kaftilous_fileops`, which both the app and `tests/` link against. Keep it free of GUI widgets so it stays testable.
- The look copies GNOME Files (Nautilus) / libadwaita, but it's built only from Qt, so it looks the same on every platform:
  - `Theme` (`src/Theme.{h,cpp}`) holds the light and dark colour tokens, chosen from `QApplication::palette()`. Use the application palette, not the widget palette: our own style sheet changes widget palettes.
  - `glyphIcon(Glyph, color)` draws every UI icon in code through a custom `QIconEngine`. Don't depend on the system icon theme or the Qt SVG plugin for UI icons. File-type icons still come from `QFileSystemModel` / the OS.
  - Light/dark preference (`ThemeMode`, set in `PreferencesDialog`) is saved with `QSettings` under `appearance/theme`. `applyThemeMode()` replaces `QApplication`'s palette with a built light or dark palette. "System" keeps or restores the platform palette that was captured at startup, so startup in System mode never calls `setPalette`. `main.cpp` applies the saved mode before creating the window.
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
- File operations (copy, cut/paste, move to trash, undo):
  - `FileJob` runs one operation on a worker thread (`FileOperations` owns a 2-thread pool). It first measures the total size, then transfers chunk by chunk, checking `cancel()`. Progress is a mutex-guarded snapshot (`progress()`); `progressChanged` is throttled and delivered queued.
  - Safety rules to keep:
    - Never overwrite: targets come from `uniqueTargetPath` ("name (copy).ext"), and files open with `QIODevice::NewOnly`.
    - A move tries `QDir::rename` first and falls back to copy and then delete; the original is deleted only after the copy is complete.
    - A cancelled or failed item removes only its own newly created target.
    - Removable drives (`needsSafeWrites`): on Linux these are udisks mounts under `/media` or `/run/media`, a USB sysfs path, or the sysfs `removable` flag; on macOS, `/Volumes/`. For those, data is flushed to the device every 32 MiB (`fdatasync`). The filesystem is synced (`syncfs`) before a cross-filesystem move deletes the source, and again at the end of the job. During that final sync the job shows `Progress::finishing` ("Writing to the drive…"), and the drive is listed in `flushedDrives()`. The internal disk is never synced, to keep it fast. `KAFTILOUS_SAFE_WRITES=1/0` forces the behaviour (used by the tests).
  - `FileOperations` records each user action's `TransferResult`s on an undo stack. Undo runs the inverse job: a copy is undone by trashing the copies (never deleting them), a move by moving back, and a trash by `Restore`, which also removes the Linux `.trashinfo`.
  - Undo jobs are marked with the `UserActionProperty` dynamic property set to false, so `MainWindow::onJobFinished` treats them differently.
  - The clipboard uses `text/uri-list` plus the GNOME (`x-special/gnome-copied-files`), KDE (cut flag) and Windows ("Preferred DropEffect") formats, so it works with other file managers.
  - UI: `OperationsPanel` (bottom-left card) appears only for jobs running longer than 400 ms. `Toast` (bottom-centre, with an Undo button for trash) avoids the panel's rectangle.
- There is no status bar. `m_floatingBar` (a `QLabel` over the view) shows selection info and temporary messages (`showMessage`).
