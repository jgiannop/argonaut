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

Linux dependencies: `cmake ninja-build qt6-base-dev qt6-tools-dev designer-qt6 qt6-wayland libvterm-dev libarchive-dev`. `libarchive-dev` is optional too ("Compress / Extract: disabled"; `src/FileJobArchive.cpp` is then left out, `KAFTILOUS_ARCHIVES` undefined, `FileJob::archivesSupported()` returns false and the UI hides the actions, and `tst_archives` isn't built). `libvterm-dev` is optional: without it, CMake prints "Embedded terminal: disabled" and the app builds without the terminal panel, with all terminal code behind `#if defined(KAFTILOUS_TERMINAL)`. If you add the terminal sources to a stale build directory and the linker reports missing `TerminalWidget` vtable/metaObject symbols, delete `build/debug/kaftilous_autogen` and rebuild.

Tests (Qt Test + CTest, in `tests/`). They run headless and sandbox `HOME` and the trash. **`KAFTILOUS_TRASH_HOME_ONLY=1` must be set for anything that runs trash code in tests:** without it, `Trash::filesDirs()` also finds the `.Trash-<uid>` folders on real mounted drives, and Empty Trash would delete them. `tst_fileoperations` sets it in `main()` and `tests/CMakeLists.txt` sets it for ctest. Tests also point `HOME`, `XDG_DATA_HOME` and `XDG_CONFIG_HOME` at a temporary folder, so the real bookmarks file and settings are never touched. Window-level test programs set `KAFTILOUS_EJECT_PROGRAM` and `KAFTILOUS_GIO_PROGRAM` to fake scripts, so they never eject a real drive, launch real apps, or change default apps.

```bash
ctest --preset debug                                    # all tests
./build/debug/tests/tst_fileoperations copyFolderTree   # one test function
```

In VS Code, CMake Tools' Testing panel runs them too. Formatting: `.clang-format` (Qt-like). Format-on-save applies to changed lines only. There is no linter.

## Architecture

- `qt_standard_project_setup()` turns on AUTOMOC/AUTOUIC. Every new source or header must be listed in `CMakeLists.txt`, or moc won't run on it. The GUI goes in `qt_add_executable(kaftilous ...)`. The folder model (`DirectoryModel`), the Trash, bookmark and Open With helpers (`Trash`, `Bookmarks`, `OpenWith`) and the file-operations engine (`FileJob`, `FileOperations`, plus `FileJobArchive.cpp` when libarchive is found) are the static library `kaftilous_core`, which both the app and `tests/` link against. Keep it free of window/widget code so it stays testable.
- Real file types (`src/ContentType.{h,cpp}`, in `kaftilous_core`): this is deliberately **stricter than Nautilus/GIO**, which trust any extension only one type uses.
  - `inspect()` reads the first 4 KiB and calls `judge(byName, byContent)`. The verdicts are Consistent, Identified (no usable name), Mismatch, and Dangerous (`isProgram(content)` while the name isn't a program).
  - Rules that prevent false alarms:
    - Generic or empty content (`application/octet-stream`, `x-zerosize`) is never a mismatch.
    - Related types (`inherits`, e.g. docx is a zip) agree with each other.
    - Two text types never count as a mismatch, because sniffing can't tell Markdown from Python.
  - A read-only scan of a real 749-file Downloads folder gave 0 false alarms and caught 1 real mismatch.
  - `DirectoryModel` applies cached verdicts while listing (`applyCachedContent`) and reads the rest on `m_contentPool` (1 thread). Reading starts only **after** the listing is done, so it never competes with it.
  - On local disks every file is checked; on removable and network drives (`m_thorough == false`) only files whose data the views ask for, i.e. the visible ones.
  - Results go to `ContentType::Cache`: QDataStream at `QStandardPaths::CacheLocation/content-types.bin`, keyed by path, size and mtime, at most 200k entries, saved 3 s after changes and on quit.
  - The effective type (`mimeFor`) is the content type for Identified, Mismatch and Dangerous. `nameMimeFor` is the name-only type. The roles are `VerdictRole`, `NameMimeRole`, `ContentMimeRole` and `Qt::ToolTipRole`.
  - UI: `FileItemDelegate::paintContentBadge` (amber for Mismatch, red for Dangerous).
    - `MainWindow::openFile` warns about Dangerous files and never runs them, and opens Mismatch/Identified files with the default app for the real type.
    - `fixExtension` ("Rename to …") is undoable, and Properties shows "Name says" against the real type.
  - Tests set `XDG_CACHE_HOME`; `ContentType::readCount()` lets them prove that a second visit reads nothing.
- Performance guards in `DirectoryModel`: each batch of a big listing is sorted on its own and **merged** (`resort(sortedPrefix)`), not re-sorted in full. `m_rowByPath` is rebuilt lazily (`rebuildIndex()` only marks it stale; readers call `ensureIndex()`). `FileEntry::fullPath` is built once (`setDir`). Re-run the A/B benchmark if you touch these: 20,000 files should load fully in about 0.2 s.
- Sizes: always use `formatSize()` (`src/Format.h`). It gives powers of 1000 with kB/MB/GB, like GNOME and drive labels; never call `QLocale::formattedDataSize`'s default, which gives KiB/MiB.
- Natural name order uses `QCollator` in numeric mode. In the "C"/POSIX locale Qt can't do numeric collation, so `naturalCollator()` uses English rules there; keep that, or `LANG=C` users get "file10" before "file2".
- The look copies GNOME Files (Nautilus) / libadwaita, but it's built only from Qt, so it looks the same on every platform:
  - `Theme` (`src/Theme.{h,cpp}`) holds the light and dark colour tokens, chosen from `QApplication::palette()`. Use the application palette, not the widget palette: our own style sheet changes widget palettes.
  - `glyphIcon(Glyph, color)` draws every UI icon in code through a custom `QIconEngine`. Don't depend on the system icon theme or the Qt SVG plugin for UI icons. File-type icons come from the OS through `QFileIconProvider` (cached per mime type by `DirectoryModel`).
  - Themes (`ThemeMode`: Follow System, Light, Dark, Midnight Blue, Rosé Dawn, Forest, Sunset) are chosen in `PreferencesDialog` and saved with `QSettings` under `appearance/theme` (keys are in `kThemes` in `Theme.cpp`).
    - Light and Dark use libadwaita's exact colours (`adwaita()`). The named themes are built by `custom()` from four base colours (window, sidebar, text, accent), and every other token is derived from those.
    - `applyThemeMode()` sets `QApplication`'s palette (`paletteFor(theme)`). Follow System keeps or restores the platform palette that was captured at startup.
    - `currentTheme()` returns the applied tokens. `MainWindow::applyTheme()` re-styles only when `Theme::id` changes.
    - To add a theme: add a `ThemeMode` value, a `kThemes` entry and a case in `Theme::forMode`. Preferences picks it up automatically.
  - `MainWindow::applyTheme()` builds one window-wide style sheet (`styleSheetFor`, using `@token` placeholders) and re-creates every themed icon. It runs again on `ApplicationPaletteChange`. Anything with a themed icon must be updated there.
- Window chrome: on Linux and Windows the window is frameless (client-side decorations). Set `KAFTILOUS_NATIVE_DECORATIONS=1` to turn that off; macOS always uses the native frame.
  - Two `HeaderBar`s, one above the sidebar and one above the content, act as the title bar. Dragging empty space calls `startSystemMove`; double-click maximises.
  - Edge resizing is handled by an event filter on the `QWindow` (`MainWindow::eventFilter` → `startSystemResize`).
  - The window buttons only exist when `m_csd` is true.
- Main view:
  - `DirectoryModel` is a flat table model (Name/Size/Type/Modified) of one folder. Don't go back to `QFileSystemModel`, which was ~5x slower on big folders and couldn't keep folders-first natural order.
    - `setPath()` lists the folder on a worker thread, using a generation counter to drop stale results. Big folders arrive in batches (the first after 15 ms). `loaded` fires when the listing is complete.
    - Each entry is stat-ed once while listing, so painting never touches the disk. Mime type, icon and type name are worked out from the name on demand and cached.
    - Sorting is done by the model: folders first (even when descending), then the sort column, then natural case-insensitive order (`QCollatorSortKey`). `resort()` keeps persistent indexes.
    - It watches only the open folder: inotify on Linux, so saved files (`IN_CLOSE_WRITE`) update too, and `QFileSystemWatcher` elsewhere. Changes are rescanned (debounced) and applied as minimal row inserts, removes and updates. More than 500 changes trigger a reset instead. `folderGone` fires if the folder disappears.
    - Sub-folder item counts ("12 items") are computed in the background.
    - Invariant: `m_rowByName` is rebuilt **before** `endInsertRows`/`endRemoveRows`, because listeners such as paste-highlight call `indexForPath` from those signals.
  - There are two views in a `QStackedWidget`: `m_iconView` (QListView, IconMode) for the grid and `m_detailView` (QTreeView with columns and sortable headers) for the list. They share the proxy model and **one selection model**. `m_view` points at the visible one.
    - Each view has its own `FileItemDelegate` (grid / list), because delegates cache sizes.
    - The table selects whole rows, so use `selectedViewIndexes()` (column 0 only) instead of `selectedIndexes()`.
    - Overlays (floating bar, progress card, toast) are children of `m_viewStack`.
  - `FileItemDelegate` paints every item itself: wrapped, middle-elided names, rounded hover/selection, list-mode size/date.
  - `Thumbnailer` loads thumbnails in a thread pool. It reads the freedesktop cache (`~/.cache/thumbnails`) on Linux first, then decodes images with `QImageReader`. It emits `thumbnailReady`, which repaints the viewport.
  - The views show `DirectoryModel` through `FileFilterProxy` (a `QSortFilterProxyModel`). Indexes from the view or selection are **proxy** indexes: convert them with `toSource()` / `toView()` before calling `m_model->filePath`/`isDir`/etc. `FileItemDelegate` resolves through proxies itself.
  - Search (Ctrl+F) is done by the model, in sub-folders too: `DirectoryModel::setSearch(query)` re-lists (`relist()`) with the worker walking breadth-first from `m_path` and keeping only names that contain **all** words, ignoring case and accents (`foldForSearch`: NFD, drop non-spacing marks, case-fold; ASCII fast path in `matchesSearch`). So Greek "θαλασσα" finds "Θάλασσα".
    - The walk doesn't follow symlinks, stays on the starting drive (`st_dev`, so no `/proc` or mounted sticks) and skips hidden folders unless hidden files are shown (`setShowHidden` searches again). It stops at `MaxSearchResults` (10,000; `searchTruncated()`). In the Trash it only filters, without recursing.
    - Rows are keyed by full path, as in trash mode. The Type column becomes Location (`searchLocation`: relative to the searched folder), and the tooltip says "In …" for the grid. Content checks only read visible results (`m_thorough = false`).
    - Only the searched folder is watched, so `onJobFinished` calls `refresh()` in search mode; the rescan is another full search applied as a minimal diff.
    - `MainWindow` debounces typing by 150 ms (`m_searchTimer`); clearing is immediate. `setPath()` ends a search, and `navigateTo` blocks the search field's signals while leaving search, so the old folder isn't listed again. The floating bar shows "Searching… / N results / No results"; "Open Item Location" goes to a result's folder and selects it.
    - `FileFilterProxy` no longer filters. It never sorts either: `FileFilterProxy::sort` forwards to the model, so header clicks and View → Sort Files both sort `DirectoryModel`. Hidden files are filtered by the model (`setShowHidden`).
    - Read-only benchmark on a real home folder: same results as `find`, first results after ~17 ms, full walk ~2 s warm (`find`: 4.4 s).
- `navigateTo(path, addToHistory)` is the **only** way to change the current folder. It validates and cleans the path into `m_current` (always with `/` separators; the native form is only for display), leaves search mode, and updates history, `PathBar`, the title, the sidebar selection and the floating bar. Everything that changes folder goes through it: actions, sidebar, breadcrumbs, the typed path, double-click and the command-line argument.
- `PathBar` shows breadcrumbs. Leading crumbs are hidden when space is short, and it switches to a `QLineEdit` on click or Ctrl+L. It emits `pathActivated`.
- Sidebar entries are `QListWidgetItem`s with the path in `PathRole` (`Qt::UserRole`). Separator rows have `SeparatorRole` and an empty path.
- File operations (copy, cut/paste, move to trash, undo):
  - `FileJob` runs one operation on a worker thread (`FileOperations` owns a 2-thread pool). It first measures the total size, then transfers chunk by chunk, checking `cancel()`. Progress is a mutex-guarded snapshot (`progress()`); `progressChanged` is throttled and delivered queued.
  - Safety rules to keep:
    - Never overwrite: files open with `QIODevice::NewOnly`. When the destination name is taken, `transferItem` resolves the clash:
      - Copies into the item's own folder duplicate without asking.
      - Otherwise `askUser` emits `conflict(source, existing)` and the worker **waits** (`QWaitCondition`) until `resolveConflict()` or `cancel()`.
      - The answers are Replace (the existing item goes to the **Trash** first; it is never deleted), Skip, KeepBoth (`uniqueTargetPath` or a typed name), Merge (folder onto folder: `mergeInto` handles each child, asking again inside) and Cancel.
      - "Apply to all" is remembered separately for files and folders.
      - Only newly created items go into `results`, so undoing a merge never touches what was already there.
      - Jobs ask only when `FileOperations::setAskOnConflict(true)`; otherwise they keep both silently, which is the tests' default. `MainWindow` shows `ConflictDialog`s one at a time (`m_conflicts` queue).
    - A move tries `QDir::rename` first and falls back to copy and then delete; the original is deleted only after the copy is complete.
    - A cancelled or failed item removes only its own newly created target.
    - Removable drives (`needsSafeWrites`): on Linux these are udisks mounts under `/media` or `/run/media`, a USB sysfs path, or the sysfs `removable` flag; on macOS, `/Volumes/`. For those, data is flushed to the device every 32 MiB (`fdatasync`). The filesystem is synced (`syncfs`) before a cross-filesystem move deletes the source, and again at the end of the job. During that final sync the job shows `Progress::finishing` ("Writing to the drive…"), and the drive is listed in `flushedDrives()`. The internal disk is never synced, to keep it fast. `KAFTILOUS_SAFE_WRITES=1/0` forces the behaviour (used by the tests).
  - `FileOperations` records each user action's `TransferResult`s on an undo stack. Undo runs the inverse job: a copy is undone by trashing the copies (never deleting them), a move by moving back, and a trash by `Restore`, which also removes the Linux `.trashinfo`.
  - Undo jobs are marked with the `UserActionProperty` dynamic property set to false, so `MainWindow::onJobFinished` treats them differently.
  - The clipboard uses `text/uri-list` plus the GNOME (`x-special/gnome-copied-files`), KDE (cut flag) and Windows ("Preferred DropEffect") formats, so it works with other file managers.
  - UI: `OperationsPanel` (bottom-left card) appears only for jobs running longer than 400 ms. `Toast` (bottom-centre, with an Undo button for trash) avoids the panel's rectangle.
- Archives (`src/FileJobArchive.cpp`, libarchive; `FileJob::Kind::Extract` / `Compress`, so they get the job engine's progress, cancel, USB safety and undo):
  - Extract unpacks each archive into a hidden `dest/.<base>.kaftilous-extract` folder, then places it: exactly one top-level item is renamed out as is, otherwise the temporary folder becomes `<base>` (both via `uniqueTargetPath`, never overwriting). Cancel or failure removes the temporary folder.
  - Security: `isUnsafePath()` rejects absolute, `..` and drive-letter paths (and hardlink targets) and counts them in one warning; `ARCHIVE_EXTRACT_SECURE_SYMLINKS | SECURE_NODOTDOT` also apply. **Not** `SECURE_NOABSOLUTEPATHS`: every entry is rewritten to an absolute path inside the temporary folder.
  - `support_format_raw` handles plain `.gz`/`.xz`; the entry is named after the archive (`archiveBaseName`, which knows `.tar.gz` and friends). Progress is compressed bytes read (`archive_filter_bytes(a, -1)`). Encrypted archives give an error (no password support yet).
  - Compress writes all sources into `dir/.<name>.kaftilous-part` and renames it when complete. The format (`ArchiveFormat`: zip with UTF-8 names, tar.gz / tar.xz as pax_restricted, 7z) goes through `FileOperations::m_pendingFormat` → `setArchiveFormat` before the job starts.
  - Undo of either trashes what was created (like Copy). `MainWindow::selectedArchives()` decides by the **effective** mime (`FileOperations::isArchive`), refusing Dangerous files. `CompressDialog` remembers the format in `archive/format`.
  - `tst_archives` covers round trips in every format, zip-slip/absolute/symlink escapes, placement, cancel (20k-entry tar, cancelled with a `DirectConnection` so it's deterministic), damaged archives and undo.
- Drag and drop:
  - Drags start in the views; `DirectoryModel::mimeData` gives one URL per item, plus text paths.
  - Drops are **not** handled by Qt's view/model logic. `eventFilter` intercepts DragEnter/Move/Leave/Drop on the views' viewports (`handleViewDrag`) and on the sidebar (`handleSidebarDrag`). The viewports have `setAcceptDrops(true)` explicitly, otherwise Qt delivers drag events to the view instead.
  - The target is the folder under the cursor (highlighted through `FileItemDelegate::setDropTarget`), otherwise the open folder.
  - `dropActionFor` decides like Nautilus: same device (cached `QStorageInfo::device`) moves, another device copies, Ctrl copies, Shift moves. It refuses drops into the item itself, drops where the items already are, and drops into read-only folders.
  - Drops go through `FileOperations`, so they get progress, undo and USB safety.
- Embedded terminal (`TerminalWidget`, Unix only): a bottom panel in `m_contentSplitter` under the file views. Ctrl+` toggles it; it is also in View → Terminal, the header button and the context menu's "Open in Terminal".
  - It runs `$SHELL` on a pty (`forkpty`). Everything the child needs is prepared **before** the fork: argv, envp (TERM=xterm-256color) and the directory bytes, because only async-signal-safe calls are allowed in the child of a multi-threaded process.
  - libvterm does the emulation (screen, alt-screen, scrollback through `sb_pushline`/`sb_popline`). Painting is cell by cell with QPainter; colours come from the theme plus the GNOME Console ANSI palette.
  - Keys: `event()` accepts every `ShortcutOverride`, so the terminal gets Ctrl+C, Delete and so on instead of the window's actions. The exceptions are `setPassthroughShortcuts` (the toggle). `focusNextPrevChild` returns false so Tab reaches the shell, and composed text arrives through `inputMethodEvent`.
  - Following the folder: `showTerminal(dir)` starts a shell in `dir`, or calls `changeDirectory(dir)`. That only types `^E^U cd -- 'dir'` when the shell is idle (`tcgetpgrp(pty) == shell pid`), never into a running program.
  - `exit` emits `finished`, which closes the panel; opening it again starts a fresh shell.
- Trash (Linux/BSD, freedesktop spec; `src/Trash.{h,cpp}`):
  - The home trash is `~/.local/share/Trash/{files,info}`. Other drives have `<root>/.Trash-<uid>` or `.Trash/<uid>`, whose `.trashinfo` `Path=` may be relative to the drive root.
  - The sidebar's Trash opens `Trash::homeFilesDir()`. `DirectoryModel` then switches to trash mode (`isTrash()`): it lists **all** trash folders merged (`m_dirs`) and keys rows by **full path** (`m_rowByPath`), because names can repeat across folders. In trash mode the Type and Modified columns become Original Location and Deleted.
  - `FileJob::Kind::Restore` recreates the original folder if it's missing and removes the `.trashinfo`; it never overwrites. `Kind::Delete` is permanent (Shift+Delete, Delete inside the Trash, Empty Trash) and is always confirmed first by `confirmPermanentDelete`.
  - Restore and Delete are user actions but aren't undoable (`startJob(..., recordUndo=false, userAction=true)`).
  - Drops onto the Trash (its sidebar entry or the view) go to `m_ops->trash()` (`performDrop`).
  - The path bar and window title show "Trash" instead of the real path.
- Sidebar sections: places + Trash, then bookmarks, then drives. Rows carry `PathRole`, `KindRole` (`PlaceItem`, `BookmarkItem` or `DriveItem`) and `EjectRole`. `populateSidebar()` rebuilds everything; `scheduleSidebarRefresh()` debounces it.
  - Bookmarks (`src/Bookmarks.{h,cpp}`) live in the GTK file Nautilus uses (`~/.config/gtk-3.0/bookmarks`, written atomically with `QSaveFile`). Lines that aren't local folders, such as `sftp://`, are preserved. Bookmarks that duplicate a place are hidden.
    - Ctrl+D toggles a bookmark, dropping folders on empty sidebar space adds them, and the sidebar's right-click menu renames or removes them.
    - The file's folder is watched, so Nautilus's edits appear.
  - Drives mounted by udisks (`/media`, `/run/media`) get an ⏏ button, painted by `SidebarDelegate` and handled in `eventFilter`. `ejectDrive` runs `gio mount --eject <root>` (`KAFTILOUS_EJECT_PROGRAM` overrides the program). It refuses while a job uses the drive, and leaves the drive first if it's the open folder.
  - Mount changes come from `POLLPRI` on `/proc/self/mounts` (a `QSocketNotifier` of type Exception) on Linux, and a 3 s poll elsewhere.
- Tabs share the **one** view and model. `m_tabs[i]` (`TabState`) stores a tab's folder, back/forward stacks, selected paths and scroll position.
  - `switchToTab` saves the active tab (`saveActiveTab`), then navigates (`navigateTo(path, false)`), restores the history stacks, and restores the selection through `selectPaths` and the scroll position through `m_pendingScroll` (applied on `loaded`).
  - `m_switchingTabs` silences `QTabBar` signals while the code edits the bar. The bar is hidden below 2 tabs. Close buttons are custom `QToolButton`s, re-iconed in `applyTheme`.
  - Ctrl+W closes a tab (the last one closes the window), Ctrl+Shift+W closes the window and Ctrl+Shift+T reopens a closed tab.
  - Middle-click opens a folder or sidebar entry in a background tab, and closes a tab when used on the tab bar. Drops onto a tab go into that tab's folder.
- Open With (Linux, `src/OpenWith.{h,cpp}`): `gio mime TYPE` runs with `LC_ALL=C` (parsed by `parseGioMime`). Apps are resolved to `.desktop` files through `QStandardPaths` (`appFromDesktopFile` reads Name/Name[lang]/Icon and skips NoDisplay/Hidden). `gio launch <desktop> <files>` starts an app and `gio mime TYPE APP` sets the default. Never pass an empty mime type; fall back to `application/octet-stream`. The UI is the context menu's "Open With ▸" (`openWithMenu`) and `OpenWithDialog`.
- Properties (Ctrl+I, Alt+Enter; `PropertiesDialog`): type, size, location, dates and free space. Folder sizes are measured on `QThreadPool::globalInstance()` and stop through a shared `atomic_bool` when the dialog closes. Permissions are shown as Nautilus-style Owner/Group/Others choices plus "executable", editable only by the owner and applied immediately with `QFile::setPermissions`.
- Typing in a file view jumps to a name (`typeAheadKey`/`typeAheadFind`: prefix match, then contains; a 1 s pause starts a new search). The mouse back/forward buttons are handled on the window's `QWindow` filter, which is always installed, not only with CSD.
- Settings (`loadSettings` at startup, `saveSettings` in `closeEvent`; `QSettings` keys `window/*` and `view/*`): window geometry, sidebar width, terminal height, list column widths, sort column and order, hidden files, sidebar visibility, and grid or list view.
- Menu bar (File/Edit/View/Go/Help, `buildMenuBar`): it sits inside the frameless window's root layout, except on macOS, where it uses the global menu bar. Its empty space drags and maximises the window like the header bars. Every command is a `QAction` member, added to the window with `addActions` so its shortcut works everywhere, and reused in the menus and the context menu. Enablement is updated in `updateEditActions()`.
- New Folder / New File create "New Folder", "New Folder 2"… then open the inline rename field (`m_pendingRename` → `startRename` once the item appears).
  - Rename (F2) is a `QLineEdit` over the item (`FileItemDelegate::nameEditorRect`), repositioned whenever the viewport paints.
  - Enter and Esc are handled in `eventFilter` and must be consumed: QLineEdit passes Enter on, and the view would then open the item.
  - Renames and new items are put on the undo stack with `FileOperations::recordUndo` (undoing a rename moves it back; undoing a new item trashes it).
- There is no status bar. `m_floatingBar` (a `QLabel` over the view) shows selection info and temporary messages (`showMessage`).
