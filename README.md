# Argonaut

A fast file manager built with C++ and Qt 6, inspired by GNOME Files (Nautilus). It is named after the Argonauts, who sailed with Jason on the *Argo* into the unknown, and after the *argonauta*, the small octopus whose shell is called the paper nautilus. It was called Kaftilous until version 0.2 and Kosmos in 0.3; settings, stars and the API key carry over by themselves.

> **Status:** preview. Used daily on Ubuntu 24.04 (X11). The code is written to be cross-platform (Linux, Windows, macOS), but only Linux has been tested so far, and the interface is in English only. **It moves and deletes real files and is young software: keep backups of anything important.** [Bug reports and feedback](https://github.com/jgiannop/argonaut/issues) are welcome.

## Features

**Browsing**
- Grid view with thumbnails, and a list view with sortable **Name / Size / Type / Modified** columns, plus optional **Owner / Group / Permissions** (right-click the header)
- **Expandable folders** in the list view (Preferences → List View): open folders in place with ▸, → and ←
- Thumbnails for photos, **PDFs, videos, fonts, comics and more**, using the thumbnailers already installed on your system (the same ones Nautilus uses) and sharing their cache. They run **sandboxed** (no network, nothing visible but the one file), two at a time at low priority, only for files on screen, and by default only on the computer's own disks (Preferences)
- Folders first, in natural order ("file2" before "file10"); sort by name, size, type or date
- Sidebar with your folders, drives and the Trash; a breadcrumb path bar (Ctrl+L to type a path)
- **Recent**: what you opened lately, in Argonaut or any other app (the list GNOME apps share), newest first. Respects GNOME's "remember recent files" privacy switch. Right-click → Remove from Recent
- **Starred**: star files and folders (right-click → Star); they get a ★ and their own place in the sidebar. Stars follow items you rename or move
- Search (Ctrl+F) in the folder **and all its sub-folders**, with results as you type. Several words narrow it down, and case and accents don't matter ("θαλασσα" finds "Θάλασσα.jpg", "cafe" finds "Café"). Right-click → Open Item Location
- **Search inside files** ("Inside files" button): text files are read as the search walks, and documents such as PDFs come from GNOME's search index; matches show the text around them
- **Search filters** by type (documents, images, audio, video, archives, text and code, folders) and by date, also without typing a name
- Show hidden files (Ctrl+H)
- **Quick Look** (Space): a preview of the selected file without opening an app: photos (animated GIFs play), PDFs, videos, fonts, text and code, the contents of archives (nothing is extracted), folders. Arrow keys go to the next file, Enter opens it, Space or Esc closes. Programs disguised as documents are never previewed. Previews are made in the same sandbox as thumbnails HTML files are shown as a page to read (text, headings, lists, tables and links; no styles, scripts or pictures, so nothing is run or fetched).
- **Split view** (F3): two folders side by side, each with its own history and view. F5 copies the selection to the other side, F6 moves it, or drag between them. Delete, rename and every other shortcut act on the side you're in
- Zoom (Ctrl+plus / Ctrl+minus / Ctrl+0, or Ctrl+mouse wheel): grid icons from 32 to 256 px, list icons from 16 to 48 px, remembered separately for each
- **Analyze view** (Ctrl+3, or the pie button): how much disk space every folder and sub-folder uses, as a tree with usage bars, biggest first. Sizes match `du` exactly (space on disk, hard links counted once; symlinks and other drives aren't followed). Folders appear while it measures; opening a folder inside what's been measured is instant. **Show Files When Analyzing** (View menu or right-click) also lists each folder's biggest files. Copying, moving, trashing, extracting and Undo update the sizes at once, without measuring everything again. Right-click to open, trash or scan again
- Live updates when files change on disk
- **Connect to Server** (sidebar, Go menu, or an address in the path bar): `sftp://`, `smb://`, `ftp://`, WebDAV and more through GVfs, with a login dialog; connected servers and phones appear in the sidebar with a disconnect button. It is an extension that comes with Argonaut, so it can be switched off if you never use it
- **Extensions**: small programs in any language that add marks to files, information to the path bar, commands to the right-click menu or the menu bar, previews for more kinds of files and results in the preview pane, switched on one by one in **Edit → Extensions** ([how to write one](docs/extensions.md)). Git support is the first; **System Dashboard** (View menu, or the command palette; in the terminal version too) shows processor, memory, disk and network use in the preview pane, updating as you watch; **Space Overview** (same places) shows how big the open folder is and the biggest folders and files inside it; **Running Services** and **Running Processes** list the running services and processes, the one using the most memory first **System health** is covered by four of them: the System Dashboard (processor, memory, load, swap, temperature, every disk, and what the drives say of themselves), Running Services (failed ones first), Running Processes, and the System Journal (this boot's errors and warnings, repeats counted); the last three have an **Analyze with Claude** button that explains the list.
- **Git repositories**: M / A / U / C badges on changed, added, new and conflicting files, a dot on folders with changes inside, and the branch in the path bar. Repositories whose own settings would make git run programs are never checked
- **Themes are files**: seven come built in, and anyone can make more: a small file of colours in the themes folder, followed live as you edit it ([how to write one](docs/themes.md)). Choose one in Preferences, View → Theme, or the command palette
- **In a terminal too**: `argonaut --tui` is the same file manager without a window, with one or two panes, a list or grid view, the mouse, and a shell panel under the files ([more](#in-a-terminal))
- Fast with big folders: 20,000 files load in about 0.15 s (one system call per file)

**Files**
- Copy, cut and paste, which also work with other file managers' clipboards
- Drag and drop: same drive moves, another drive copies, Ctrl copies, Shift moves
- Progress for long operations, with time left and cancel
- Undo (Ctrl+Z) and Redo (Ctrl+Shift+Z or Ctrl+Y) for copy, move, trash, rename, new items, extract and compress. Redo brings back the very same items (for example, from the Trash), it never copies again
- Never overwrites: name clashes become "name (copy)"
- New Folder, New File, Rename (F2), and **New Document** from your Templates folder
- **Batch rename** (F2 on several items): a template with the original name and numbers, or find and replace; extensions are kept, a live preview shows clashes, one Ctrl+Z undoes it
- **Select Items Matching** (Ctrl+S, e.g. `*.jpg`) and **Invert Selection** (Ctrl+Shift+I)
- Hold a dragged item over a folder for a moment and it opens, so you can drop deeper
- USB sticks: Argonaut says "done" only after the data is really written to the drive

**Archives** (with libarchive)
- Right-click → **Extract Here** / **Extract to…** for zip, 7z, tar.gz/xz/bz2/zst, rar, iso, deb, plain .gz/.xz and more
- Smart like Nautilus: one item inside comes out as is; several go into a folder named after the archive
- Safe: never overwrites, and entries that try to escape the folder (`../`, absolute paths, symlink tricks) are skipped and reported
- Uses the real type: a `.zip` that is really 7z still extracts; a program named `.zip` is never treated as an archive
- **Compress…** any selection to .zip, .tar.xz, .tar.gz or .7z
- Progress, cancel (nothing half-finished is left behind) and Undo, like copying

**Real file types: safer than trusting names**

Most file managers, Nautilus included, believe a file's extension: `report.pdf` is shown and opened as a PDF without anyone looking inside. Argonaut checks what each file really is, from its first few kilobytes, and tells you when the name lies:
- An **amber ⚠ badge** marks files whose content doesn't match their name, for example a `.pdf` that is really an HTML error page from a failed download, or a `.zip` that is really a 7z archive. Hover for details. Right-click → **Rename to “report.html”** fixes it (and can be undone).
- A **red ⚠ badge** marks **programs disguised as documents** (`invoice.pdf` that is really an executable). Opening one shows a warning; Argonaut never runs it.
- Files are **opened with the app for what they really are**, so a mislabelled file still opens correctly.
- Files with no extension get their real type and icon.
- **Properties** shows both what the name says and what the file is.
- **It doesn't slow anything down.** The folder appears instantly from the names, the check runs in the background, and the results are remembered (in `~/.cache`), so each file is read only once until it changes. On USB sticks and network drives only the files on screen are checked.

**Trash** (Linux)
- Restore to the original location (or **Restore All**), Delete Permanently (Shift+Delete), Empty Trash
- Shows items trashed on all drives, including by other apps

**Extras**
- Embedded terminal (Ctrl+`) that opens in the current folder
- Themes: Follow System, Light, Dark, Midnight Blue, Rosé Dawn, Forest, Sunset (Preferences, Ctrl+,)
- Tabs (Ctrl+T), bookmarks shared with Nautilus (Ctrl+D), a PDF reader in Quick Look (every page, drawn in a sandbox), eject for USB drives, drives that aren't mounted in the sidebar (a click mounts one), Open With…, Properties (Ctrl+I), type-to-find
- **Properties → Details** for photos, music, videos and PDFs (dimensions, camera and exposure, date taken, length, tags, pages), read with exiftool inside a sandbox
- 🔒 and ↗ badges on items you can't change and on links; Cut, Trash and Rename are off where the folder can't be changed
- **Send by Email…**, **Set as Background**, and **Scripts ▸** (runs your GNOME Files scripts unchanged)
- **Command palette** (Ctrl+Shift+P): type part of any command
- A built-in **User Guide** (F1) and a **Keyboard Shortcuts** page (Ctrl+?)
- Sizes in GNOME's units (kB, MB, GB), matching Nautilus and drive labels

**Organize with Claude** (optional, needs your own Anthropic API key)

File → **Organize with Claude…** asks Claude, Anthropic's AI, to propose folders, better names and a short report for a messy folder. The plan appears in the preview pane as a checklist; you apply what you keep, and Ctrl+Z puts everything back. It is an extension (switch it off in Edit → Extensions), and it never has your key: Argonaut asks Claude for it. See [Claude and your privacy](#claude-and-your-privacy).

**System cleanup.** View → **System Cleanup**: what can safely be removed to free disk space, biggest first: packages no longer needed, old kernels, downloaded package files, old Snap revisions, Docker's unused images and stopped containers, the journal's older part, crash reports, your caches and the Trash. It removes nothing by itself: the system's part is shown as the commands to run, and your caches go to the Trash only from a checklist you tick, with Ctrl+Z to bring them back.

**Updates and restart.** View → **Updates and Restart**: the updates waiting (system packages, Snaps, Flatpaks, firmware), security fixes first, whether the computer needs a restart and why, and which programs still run libraries that were updated. It installs nothing: the commands are shown to run yourself, and Explain with Claude says what the updates are about.

**How to install.** File → **Ask Claude: How to Install…**: name a program ("mysql server", "node 22") and Claude says the best way to install it on this computer, with the commands, how to check it and how to remove it. It looks up on your machine whether the package is installed and which version your system offers, so the answer is about your system. It only advises: Argonaut runs nothing.

**Network connections.** View → **Network Connections**: the interfaces with their speed, the programs that listen for connections (a port other machines can reach is marked) and the programs that are connected, with where to. Searchable, updated every three seconds, with Analyze with Claude under it. It only shows; how much each program sends is something the system tells only the administrator.

**Tell Claude what to do.** File → **Tell Claude What to Do Here…**: say in your own words what should be done in the open folder ("move every PDF older than a year into Archive", "make a zip of the photos from August"). Claude proposes steps; you see them as a checklist and apply what you keep, and Ctrl+Z undoes it. Claude changes nothing itself: a step can only move, rename, copy, compress, make a folder or move to the Trash, only for items of the open folder, only in your own folders and drives. Nothing is ever deleted for good.

**What is this about?** Right-click any file or folder and choose **Ask Claude: What Is This About?** for a few lines saying what it is and what is in it: text and code, PDFs, office documents, pictures, archives, folders, music and video. It is an extension too, and uses the same key.

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

## Claude and your privacy

The Claude features are off until you add an API key in **Preferences → Claude** (it is stored in your desktop keyring, or read from `ANTHROPIC_API_KEY`). Nothing is sent until you start a task yourself. Organize shows the estimated cost before it starts; the report on running services is a single request, and says what it cost under the report.

What is sent to Anthropic's API when you organize a folder: the names, sizes, dates and types of its items (up to 500), and, only if you allow it, the beginning of some files (some text, the first two pages of a PDF, a small preview of an image). Programs are never read. **Preferences → Claude** sets the default: names only, ask before reading files (the default), or read files when needed. Requests are billed to your own Anthropic account.

**Every answer is kept on your disk.** Whatever asks Claude for you (Organize, the reports on services, processes and the journal, any extension), each answer is written as a Markdown page in `~/.local/share/argonaut/claudeReports` (**Go → Claude Reports**), named after what asked and when, for example `running_services_report_17.11.26_12.54.31.md`. It holds Claude's answer and, under it, everything that was sent: the instructions given to Claude, the whole list or conversation, what you typed with it, and the settings. Pictures and files that went along are named with their size, not copied, and the API key is never in it. Only you can read the files, and Argonaut never deletes them.

What is sent when you press **Analyze with Claude** under the list of running services: each service's name, description, memory, whether it is the system's or your session's, the path of the program it runs and of its unit file, and whatever you typed beside the button. A program's arguments are never sent (they can hold passwords). Under the list of running processes the same button sends, for the 400 biggest processes: the name, the process ID, the ID of the process that started it, whose it is, its memory and the program that was started; never how it was started (its arguments). **Tell Claude What to Do Here…** sends your sentence and the names, sizes, dates and types of the open folder's items (500 at most), never what is in the files.

**Ask Claude: What Is This About?** (right-click a file or folder) sends what it reads of that one item: the first 24 kB of a text file, the words of a web page, the text of a PDF's first pages or of an office document (both read in a sandbox), a copy of a picture made smaller (1024 pixels at most), the names and sizes inside an archive or a folder, or a music or video file's details. A small PDF with no readable text is sent whole. Programs, and files that only look like documents, are never read or sent. The line under each answer says what was sent.

Under Updates and Restart, **Explain with Claude** sends the waiting updates' names, versions and sources, your system's name and kernel, and why a restart is needed.

**Ask Claude: How to Install…** sends what you typed, your system's name and version, its processor type, the desktop, which package tools it has, and for the packages Claude asks about, whether they are installed and which version your system offers. Nothing about your files.

Under Network Connections the button sends the ports that are open and the names of the programs behind them, this computer's network addresses, its gateway and DNS servers, and for each connected program the addresses and ports it is connected to (40 at most a program), as numbers: no names are looked up. That shows which services you use, so look at the list first. Never how a program was started, and never what is sent over a connection (Argonaut can't see that).

Under the System Journal the button sends the messages themselves, as they are, for the 150 most important kinds of message, with who wrote each, how often and when: journal messages can name users, computers, network addresses and files, so look at the list first. Claude's report is its reading of what it was sent, not a security scan: it sees names, paths and messages, not what the programs do.

## Building

### Requirements

- A C++17 compiler (GCC, Clang or MSVC)
- CMake 3.21 or newer, and Ninja
- Qt 6.4 or newer (Widgets; Test for the tests)
- Optional: **libvterm** for the embedded terminal (Linux/macOS). Without it, Argonaut builds without the terminal.
- Optional: **libarchive** for Compress / Extract. Without it, those menu items are hidden.

### Linux (Ubuntu / Debian)

```bash
sudo apt install build-essential cmake ninja-build qt6-base-dev qt6-tools-dev qt6-wayland libvterm-dev libarchive-dev libncurses-dev
```

### Windows and macOS

Install Qt 6 with the [Qt Online Installer](https://www.qt.io/download-qt-installer), plus CMake and Ninja. If CMake can't find Qt, point it at your installation with `-DCMAKE_PREFIX_PATH=<path-to-Qt>/6.x.x/<compiler>`.

### Build, run, test

```bash
cmake --preset debug
cmake --build build/debug
./build/debug/argonaut [folder]

ctest --preset debug      # run the tests
```

For an optimized build, use `release` instead of `debug`.

## Installing (Linux)

### From a .deb package (Ubuntu 24.04, Ubuntu 26.04, Debian 13)

The [releases page](https://github.com/jgiannop/argonaut/releases) has a package for each system, named after it: `argonaut-files_<version>_ubuntu24.04_amd64.deb`, `…_ubuntu26.04_amd64.deb`, `…_debian13_amd64.deb`. Take the one for yours (`lsb_release -ds` says which it is): a package only installs on the system it was built for, because the libraries it needs are named differently on each. Then:

```bash
sudo apt install ./argonaut-files_0.15.0_ubuntu24.04_amd64.deb
```

The package also brings a small AppArmor profile (`/etc/apparmor.d/argonaut-files`): Ubuntu 24.04 and later let a program make a sandbox only if a profile says it may, and Argonaut reads files it doesn't trust (for thumbnails, previews and details) only inside one. An Argonaut built from source and started from the desktop has no such profile there, and those features show nothing; **Help → About → System** says whether the sandbox works.

On a server without a desktop, add `--no-install-recommends` and run `argonaut-tui`: the terminal version needs no display.

apt also offers the helpers Argonaut uses when they're there (GVfs for servers, bubblewrap for its sandbox, poppler-utils and exiftool for details, Tracker for searching inside documents).

To build the package yourself:

```bash
cmake --preset package
cmake --build build/package
cpack --config build/package/CPackConfig.cmake   # build/package/argonaut-files_<version>_amd64.deb
```

### From source, for your user only

Argonaut can also install for your user only, in `~/.local`: no `sudo`, nothing outside your home folder.

```bash
cmake --preset release
cmake --build build/release
cmake --install build/release      # ~/.local/bin/argonaut, menu entry, icon
```

It then appears in the app grid as **Argonaut**. To uninstall: `cmake --build build/release --target uninstall`.

### Using it instead of Nautilus

Open **Preferences → Default File Manager → Make Argonaut the Default**, or run `argonaut --make-default`. Argonaut then opens:
- folders opened from other apps, desktop icons, Super+E and "Open containing folder" (it becomes the default app for folders);
- "Show in folder" from Firefox, Chrome and others (it answers `org.freedesktop.FileManager1` on D-Bus).

To go back: **Preferences → Use Nautilus Again**, or `argonaut --restore-nautilus`, which works from any terminal even if Argonaut itself won't start. Nautilus is never removed or changed; switching only points these two hooks at one or the other, and the previous file manager is remembered.

Notes:
- If Nautilus is already running in the background, "Show in folder" keeps going to it until it stops (`nautilus -q`, or log out and in once).
- The dock's **Files** icon still starts Nautilus: right-click it → *Unpin*, then right-click Argonaut → *Pin to Dash*.

### In a terminal

`argonaut --tui` (or `argonaut-tui`) is Argonaut without a window: the files in the terminal, in one pane or two side by side, for a server reached over SSH or for working from the keyboard. It uses the same engine as the window, so copying and moving have progress and undo, deleting goes to the Trash, anything permanent is asked about first, and Git repositories show their marks and branch.

```bash
argonaut --tui                 # this folder, in one pane (F4 opens a second)
argonaut --tui ~/src /mnt/usb  # two folders, side by side
```

| Keys | |
|---|---|
| Arrows, PgUp/PgDn, Home/End | move |
| Enter / Backspace | into a folder, or open the file with its app / up (Enter on `..` goes up too) |
| F3 | the preview at the right: what is in the file under the cursor (text, an archive's contents, a rough picture). Tab goes to it; there the arrows and PgUp/PgDn scroll, or use the wheel |
| F12 or Ctrl+` | a terminal under the files (about a third of the height), in this folder; again: back to the files (the terminal stays). In it, Ctrl+Up brings the files to its folder and `exit` closes it |
| F4 | a second pane beside the first, or back to one |
| Alt+F, Alt+E, Alt+V, Alt+G, Alt+H | the menus at the top (File, Edit, View, Go, Help), or click one; arrows, Enter, Esc |
| View → Effects | on by default: exact theme colours, names coloured by kind, hover, rounded menus with a shadow, smooth bars; off for the plain look or a slow line |
| Ctrl+P | the command palette: type a few letters of what you want ("new", "trash", "big") and press Enter. Themes are there too ("theme forest"): the window's themes and your own theme files, or the terminal's own colours |
| Tab / Shift+Tab | the next of the panes and the places at the left |
| in a box an extension opens | Connect to Server's login and Organize with Claude's questions: Tab or the arrows go round the fields and buttons, Space changes a tick or a choice, Enter goes on, Esc cancels. Claude's plan is a list to tick: Space ticks, `a` is all or none, Enter applies (Ctrl+Z undoes it), Esc changes nothing. Edit → Claude API Key… is where the key is typed on a machine without the window |
| Ctrl+B | show or hide the places at the left (Recent, Starred, Home…, Trash, bookmarks, drives); in them Enter, or a click, opens one in the pane |
| F9 | the pane as a list with sizes and dates, a grid of names, or big icons, in turn (in a grid the arrows move four ways, Enter opens and Backspace goes up) |
| Space or Insert | select |
| drag the mouse with the button held | select everything the rectangle covers, as in the window (past the top or bottom the list scrolls on); Ctrl+click adds or removes one item, Ctrl+drag adds, a click on empty space clears |
| Ctrl+C / Ctrl+X, then Ctrl+V | copy / cut the selection, go anywhere, paste. With a desktop and `xclip` (or `wl-clipboard`) the clipboard is shared with the window version and other file managers; over SSH it is Argonaut's own |
| F5 / F6 | copy / move to the other pane |
| F7 / F2 | new folder / rename |
| F8 or Delete | move to the Trash |
| Shift+Delete | delete forever (asks) |
| Ctrl+Z / Ctrl+Y | undo / redo |
| a letter or a digit | go to the name that starts with what you type; the same letter again goes to the next such name |
| Ctrl+L | go to a folder (`trash` for the Trash) |
| . | hidden files |
| F1 | all keys |
| Ctrl+Q or F10 | quit (GNOME Terminal keeps F10 for its own menu, so use Ctrl+Q there) |

A scrollbar at a pane's right edge shows where you are in a long folder; a click on it goes to that part. The mouse works too: click a row to go to it, double-click a folder or a file to open it (`..` or the folder's name at the top of the pane: up), right-click for a menu of what can be done with the item (it becomes the selection, as in the window; Space selects several), scroll with the wheel, and click a word on the bottom line ("F5 Copy"). While Argonaut has the mouse, hold Shift to select text the terminal's own way.

Opening a file starts the app that is the default for what the file really is, as the window does; a program disguised as a document is never opened. Where there is no desktop (a server over SSH), a text file is shown in the terminal with `less` and other files can't be opened.

It is a first version: no menus, search, tabs or previews yet. It needs no display, but the Qt libraries must be installed (they come with the package).

### Open and Save windows for other apps

Apps like Firefox, Signal and Chrome ask the desktop for their **Open** and **Save** windows. **Preferences → Open and Save Windows → Use Argonaut for Open and Save** makes those Argonaut windows: your sidebar, bookmarks, search and views, the app's file types and options at the bottom, and a question before a file is replaced. They also open much faster than GNOME's own on X11: about 0.2 s against several seconds for a big folder ([measurements and the reason](docs/compare.md)).

It works with the `.deb` package (the desktop portal only looks in `/usr/share`), and it changes only your own `~/.config/xdg-desktop-portal/` settings; the same button switches back.

## Developing in VS Code

Open the folder in VS Code and install the recommended extensions when asked (C/C++, CMake Tools, Qt). Pick the **Debug** preset, then press **F7** to build or **F5** to run with the debugger. Tests appear in the Testing panel.

## Project layout

```
src/               Application source code
src/tui/           The terminal interface (argonaut --tui)
extensions/        The extensions that come with Argonaut (Git, System Dashboard, Space Overview, Running Services, Running Processes, System Journal, Network Connections, System Cleanup, Updates and Restart, How to Install, What Is This About, Tell Claude What to Do, Document Preview…)
docs/              The guide for writing extensions; the comparison with GNOME's file dialog
tests/             Qt Test suites (run with ctest)
CMakeLists.txt     Build definition
CMakePresets.json  Debug and release build and test presets
.vscode/           VS Code build, debug and formatting settings
CLAUDE.md          Architecture notes for contributors
```

## License

Copyright © 2026 johngiannop.

Argonaut is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version. It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; see [LICENSE](LICENSE) for the full text.
