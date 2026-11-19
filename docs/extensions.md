# Writing an Argonaut extension

An extension is a small program that adds to what Argonaut's file views show.
It can be written in any language. Argonaut starts it and talks to it in JSON.
The Git marks (M / A / U / C and the branch in the path bar) are an extension:
its source is in [`extensions/git`](../extensions/git).

Version 1 of the protocol lets an extension:

- put a **mark** on items in a folder: one or two characters, or a dot, in one
  of five colours, with a tooltip;
- show a **short text** for the folder at the right end of the path bar;
- add **commands** to the right-click menu, in submenus if there are many,
  ask the user for a text first (a message, a name), say a line when one is
  done, and show a longer result (a diff, a report) in the preview pane;
- keep a **list of files** that the file pane shows like a folder (Git's
  changed files), with your own preview for each file in it;
- supply the **preview** (the pane Space opens) for kinds of files Argonaut
  can't show itself, as text, formatted text or a picture.

Ten extensions come with Argonaut and are good to read: Connect to Server
([`extensions/connect`](../extensions/connect): rows in the sidebar, a kind of
address that is its to open, and forms for the questions a server asks),
System Dashboard
([`extensions/dashboard`](../extensions/dashboard): a picture it draws itself,
shown as a result that updates while it is on screen), Space Overview
([`extensions/space`](../extensions/space): the open folder's size and the
biggest things inside it, a result that fills in while the folder is
measured), Running Services
([`extensions/services`](../extensions/services): systemd's running services,
most memory first: a picture in the window, a list in the terminal), Running
Processes ([`extensions/processes`](../extensions/processes): the same for
every process, read from `/proc`), Organize with Claude
([`extensions/organize`](../extensions/organize): it asks Claude through
Argonaut and proposes a plan that Argonaut shows and carries out), Document Preview
([`extensions/preview`](../extensions/preview): Markdown, CSV and
spreadsheets in the preview pane, with the parsing done in a sandbox), Git
([`extensions/git`](../extensions/git): marks, the path bar text and commands
that depend on the folder) and Send by Email and Set as Background
([`extensions/desktop`](../extensions/desktop): a command and nothing else, in
about 80 lines).

Extra list columns and details in Properties are planned.

## Safety, for you and for the people who use your extension

An extension is an ordinary program with the user's rights. Argonaut does not
sandbox it. So:

- An extension from the user's own folder **never runs until the user
  switches it on** in the Extensions window (Edit → Extensions, Ctrl+Shift+X).
- Argonaut treats everything an extension sends as data. Texts are cut to
  size, unknown values are replaced, and marks for items outside the folder
  that was asked about are dropped.
- If an extension's folder is removed, it is stopped the next time Argonaut
  looks (when the Extensions window opens, or at the next start) and its switch is
  forgotten: a folder with the same id that appears later starts off.
- An extension that crashes is started again twice, then left off until it is
  switched off and on again or Argonaut restarts. A crash or hang never stops
  Argonaut.
- A command is carried out **by your program**, with the paths Argonaut hands
  it. Extensions can't use Argonaut's own copying, moving, Trash or Undo, so
  what a command does can't be undone with Ctrl+Z. If it changes or removes
  anything, give it a `confirm` question, and prefer moving things to the
  Trash (`gio trash`) over deleting them.
- Don't run tools on a folder's files that could execute what is in them.
  The Git extension, for example, refuses repositories whose settings would
  make `git status` start other programs.

## Where an extension lives

A folder named after the extension, holding `extension.json` and the program:

```
~/.local/share/argonaut/extensions/
    word-count/
        extension.json
        word-count.py
```

(`$XDG_DATA_HOME/argonaut/extensions` if that variable is set.) Extensions that
come with Argonaut are in `/usr/share/argonaut/extensions`. To try a folder of
extensions without installing them, start Argonaut with
`ARGONAUT_EXTENSIONS_PATH=/some/folder` (several folders separated by `:`);
only those are looked at then.

### extension.json

```json
{
    "id": "word-count",
    "name": "Word Count",
    "description": "Marks long text files.",
    "version": "1.0",
    "author": "You",
    "protocol": 1,
    "exec": ["word-count.py"]
}
```

| Key | Meaning |
|---|---|
| `id` | Required. Letters, digits, `.`, `-`, `_`; at most 64 characters. If two extensions share an id, the user's copy wins. |
| `protocol` | Required. `1`. |
| `exec` | Required. The program and its arguments, as a list (or one string for a program alone). A relative program is inside the extension's folder. It must be executable. |
| `name`, `description`, `version`, `author` | Shown in the Extensions window. |
| `start` | `"when-needed"`: your program isn't started with Argonaut, only when something of yours is used. See [Starting only when needed](#starting-only-when-needed). |
| `commands`, `previews` | For a `when-needed` extension: what you offer, so it can be shown while your program isn't running. |
| `uses` | `["claude"]`: you ask Claude through Argonaut ([Asking Claude](#asking-claude)). Shown in the Extensions window. |
| `addresses` | The kinds of address that are yours to open, by what comes before `://`: `["sftp", "smb"]`. See [Addresses](#addresses). |

### Starting only when needed

Every running extension is a program in memory. If yours is used now and
then (a command in a menu, a preview for one kind of file), ask to be
started only then:

```json
{
    "id": "word-count",
    "protocol": 1,
    "exec": ["word-count.py"],
    "start": "when-needed",
    "commands": [{"id": "count", "label": "Count Words", "kind": "files", "types": ["text/*"]}],
    "previews": ["text/x-my-format"]
}
```

`commands` is what a `commands` message would hold, and `previews` the
`types` of a `previews` message. Argonaut shows them from this file. When
the user chooses one of the commands, previews a file of one of the types,
or opens one of your `addresses`, your program is started, gets `hello` and
then the message, exactly as if it had been running all along. What you
then say yourself (`commands`, `previews`) replaces what the file said.

After about a minute with nothing to do, the program is stopped (its input
is closed; exit when that happens) and the file speaks for it again. It is
not stopped while a named result of yours is on screen, a form is open, or
you have sidebar rows or lists.

Don't ask for this if you put marks on files or text in the path bar: for
that you must be running whenever a folder is shown.

## The conversation

Argonaut starts the program once, when the first folder is shown, with the
extension's folder as the working directory and `ARGONAUT_EXTENSION_ID` set.
Messages are JSON objects, **one per line**, UTF-8: Argonaut's arrive on
standard input, yours go to standard output. Flush after every line. Standard
error is thrown away. When standard input ends, Argonaut is done with you:
exit.

Ignore message types and keys you don't know; newer versions may add some.

### From Argonaut

```json
{"type": "hello", "protocol": 1, "app": "argonaut", "version": "0.16.0", "interface": "window"}
{"type": "watch", "folder": "/home/me/Documents"}
{"type": "refresh", "folder": "/home/me/Documents"}
{"type": "unwatch", "folder": "/home/me/Documents"}
{"type": "invoke", "command": "mark-done", "folder": "/home/me/Documents",
 "selection": ["/home/me/Documents/report.txt"], "input": ""}
```

- `watch`: a view now shows this folder. Answer with a `folder` message, now
  or when you know; send another whenever your answer changes.
- `refresh`: something in it may have changed (a file was saved, a copy
  finished, the window came back to the front). Look again; answer if
  anything is different.
- `unwatch`: no view shows it any more. Forget it.
- `preview`: the user is previewing a file of a kind you announced (see
  [Previews](#previews)).
- `address`: the user asked for an address of a kind you said is yours (see
  [Addresses](#addresses)).
- `form`: the answer to a form you showed (see [Forms](#forms)).
- `place`: the user clicked one of your sidebar rows, or its ⏏ (see
  [Places in the sidebar](#places-in-the-sidebar)).
- `result`: a result you named is on screen now, or no longer is (see
  [Results that update](#results-that-update)).
- `invoke`: the user chose one of your commands (and said yes to its
  question, if it has one). `selection` holds the full paths of the selected
  items; it is empty for a command on the folder itself. `folder` is the open
  folder, or `""` in Recent and Starred. `input` is what the user typed, for
  a command with `ask` (otherwise empty).

Folders are absolute paths. The Trash, Recent and Starred are never sent. While searching, the folder is the one being
searched and its results are items deeper inside it.

### To Argonaut

```json
{"type": "folder",
 "folder": "/home/me/Documents",
 "info": {"text": "3 drafts", "tooltip": "Three files are still drafts."},
 "badges": {
     "report.txt":      {"text": "D", "color": "warning", "tooltip": "Draft"},
     "archive":         {"text": "",  "color": "dim", "deep": true},
     "notes/today.txt": {"text": "D", "color": "warning"}
 }}
```

Each `folder` message **replaces** what you said about that folder before.
Messages about folders you were not asked to watch are ignored.

| Key | Meaning |
|---|---|
| `info.text` | Up to 60 characters at the right end of the path bar. Leave `info` out for nothing. |
| `info.tooltip` | Up to 300 characters. |
| `info.icon` | `"branch"`, or leave it out. |
| `badges` | Keys are paths **relative to the folder**, `/`-separated, at any depth (deeper ones show in expanded folders and search results). No leading `/`, no `..`. At most 50,000. |
| `text` | At most 2 characters. Empty draws a dot. |
| `color` | `"warning"` (orange), `"success"` (green), `"error"` (red), `"accent"` (the theme's colour) or `"dim"` (grey). |
| `tooltip` | Added to the item's tooltip. |
| `deep` | `true`: the mark is also for everything inside this folder. A key of `""` with `deep` marks every item in the watched folder. |

If several extensions mark the same item, the first one in the Extensions window's list
is shown.

### Commands

Commands that make sense everywhere are announced once, after `hello`:

```json
{"type": "commands", "commands": [
    {"id": "resize", "label": "Make Smaller…", "kind": "files", "types": ["image/*"],
     "confirm": "Replace these pictures with smaller ones?"},
    {"id": "serve", "label": "Share This Folder", "target": "folder"}
]}
```

Commands that depend on the folder go in its `folder` message, under the
same `"commands"` key, and are replaced with it. Git does this: Stage and
Unstage exist only inside a repository, and only for changed items.

| Key | Meaning |
|---|---|
| `id` | Required. Comes back in `invoke`. |
| `label` | Required. Up to 60 characters. |
| `target` | `"selection"` (default): shown when items are right-clicked. `"folder"`: shown on the folder's empty space. |
| `menu` | `"view"` (or `"file"`, `"edit"`, `"go"`): the command goes at the end of that menu of the menu bar, and so into the command palette (Ctrl+Shift+P), **instead of** the right-click menu. For a command that isn't about any item or folder (a dashboard). Only in a `commands` message. `folder` in its `invoke` is the open folder, `selection` is empty. A name Argonaut doesn't know: the command isn't shown. |
| `kind` | `"files"`, `"folders"` or `"any"` (default): what every selected item must be. |
| `multiple` | `false`: only when exactly one item is selected. Default `true`. |
| `types` | Every selected item's type must match one of these: `"image/*"`, or a type such as `"text/plain"` (types derived from it match too). The type is what the file really is, by its content, not only its name. |
| `items` | Only in a `folder` message: the command is offered only when every selected item is in this list of relative paths. |
| `confirm` | A question Argonaut asks first, with Cancel as the default. Without a yes you get no `invoke`. |
| `ask` | `{"label": "Commit message:", "multiline": true, "text": "starting text"}`: Argonaut shows a text field first and sends what was typed as `input`. If the user cancels or types nothing, you get no `invoke`. |
| `group` | The submenu the command is in: `"Git"`, or `"Git/Switch Branch"` for a submenu inside one. Two levels at most. |
| `checked` | `true`: shown ticked (the current one of several, such as the branch you are on). |

A click on your path-bar text (`info`) opens a menu of your commands for the
folder (`"target": "folder"`), so that is the natural place for them; when
they are all in one group, the menu shows the group's entries directly.

At most 20 commands everywhere and 80 per folder. Commands aren't offered in
the Trash or in other programs' Open and Save windows.

To tell the user how it went, at any time:

```json
{"type": "message", "text": "3 pictures made smaller"}
```

It is shown for a few seconds at the bottom of the window (up to 200
characters, one line). Use it for results and errors, not for progress.

For a result that needs room, ask for it to be shown in the preview pane:

```json
{"type": "show", "title": "Changes on main", "kind": "diff",
 "text": "diff --git a/notes.txt b/notes.txt\n@@ -1 +1 @@\n-old\n+new", "note": "1 changed file"}
```

`kind`, `text` and `note` are as in a [preview](#previews) answer (`text`,
`markdown`, `html`, `diff`, `image`). The pane opens if it is closed, and the
next file the user selects takes it back.

#### Results that update

A result that changes while the user looks at it (a dashboard, the progress
of your own work) gets a name:

```json
{"type": "show", "id": "dashboard", "title": "System Dashboard",
 "kind": "image", "image": "/run/user/1000/my-dashboard.png"}
```

Argonaut then tells you when it is on screen, how many pixels there are for
it and whether the window is dark, again when that room changes, and once
more when it is gone (a file was selected, the pane was closed):

```json
{"type": "result", "id": "dashboard", "shown": true, "width": 420, "height": 800, "dark": true,
 "colors": {"window": "#242424", "surface": "#303030", "text": "#ffffff", "dim": "#a0a0a0",
            "accent": "#3584e4", "border": "#3a3a3a"},
 "font": "Ubuntu"}
{"type": "result", "id": "dashboard", "shown": false}
```

`colors` and `font` are the window's theme where your result is shown:
the background it sits on (`window`), a panel on that background
(`surface`), text and secondary text, the accent colour and a border
colour, all as `#rrggbb`. Draw your picture with them and it belongs in the
window in every theme; you are told again when the user picks another theme.

While it is shown, send a new version as often as you need, with
`"update": true`:

```json
{"type": "show", "id": "dashboard", "update": true, "title": "System Dashboard",
 "kind": "image", "image": "/run/user/1000/my-dashboard.png"}
```

#### In the terminal interface

Argonaut also runs in a terminal (`argonaut --tui`), where there are
character cells and no pixels. `hello` says which it is: `"interface"` is
`"window"` or `"terminal"`. There, a result or preview is shown as text, and
a picture only as a rough mosaic, so an extension that draws should answer
with text of its own colours instead:

```json
{"type": "show", "id": "dashboard", "title": "System Dashboard", "kind": "ansi",
 "text": "\u001b[1mCPU\u001b[0m  \u001b[38;5;6m████\u001b[38;5;238m██████\u001b[0m  42%"}
```

`ansi` is text with colour codes (`ESC [ … m`: bold, underline, reverse, the
16 and the 256 colours, and full colour, shown as the nearest of 256). Every
other escape sequence is dropped, so you colour your text but don't drive
the terminal. A line longer than the pane is cut; more lines than fit
scroll. In the terminal, the `result` message has the room in characters
instead of pixels:

```json
{"type": "result", "id": "dashboard", "shown": true, "width": 0, "height": 0, "dark": true,
 "columns": 44, "rows": 23}
```

If you send `ansi` to the window, it shows the text without the colours.
In both command palettes your menu commands are named with `ext::` in front
(`ext::System Cleanup`), so users can tell them from Argonaut's own; the
window's palette also names your extension beside it. The terminal's command
palette (Ctrl+P) lists the commands you put in the window's menus, and asks their `confirm` and `ask` questions itself.

An update only replaces your result where it is shown. It never opens the
pane and never takes it from a file, so a late one does no harm; but stop
when you are told `"shown": false`, and don't update more often than the
user can read (the dashboard does it every two seconds). A picture drawn at
exactly `width` pixels is shown as it is, not scaled.

**A result the user can search.** Put `"search"` in the `show` of a named
result (its value is what the empty field says) and Argonaut puts a search
field over it:

```json
{"type": "show", "id": "processes", "title": "Running Processes",
 "kind": "image", "image": "/run/user/1000/my-list.png", "scroll": true,
 "search": "Search processes"}
```

What the user types comes to you once the typing pauses, and again, with
an empty text, when the field is emptied:

```json
{"type": "search", "id": "processes", "text": "chrom"}
```

Answer by making the result again (an `update`) with only what fits. The
search is yours to do: Argonaut doesn't know what your picture shows. The
field goes when the result goes (`"shown": false`): forget the text then.
In the terminal interface the field is a question at the bottom, asked
when the user presses `/` in the preview.

**A button under a result.** A named result can also offer one thing to do with it: add `"action"` to the `show`, and Argonaut puts a button under the result, with a text field at its left when you give `"input"` (what the empty field says):

```json
{"type": "show", "id": "services", "title": "Running Services", "kind": "image", "image": "…", "scroll": true,
 "action": {"id": "analyze", "label": "Analyze with Claude", "input": "Anything in particular? (optional)"}}
```

When it is pressed (or Enter is pressed in its field) you get:

```json
{"type": "action", "id": "services", "action": "analyze", "text": "only the bus service"}
```

**Text under a result.** What you have to say about a result that is on screen (a report on the list above) doesn't have to replace it: send a `show` of kind `text`, `markdown` or `html` with `"under"` set to that result's id, and Argonaut splits the pane and shows it below the result, which stays (and keeps taking your updates). Its `note` is shown under it, and the user can close that part or drag the divider. Where that result is no longer on screen, and in the terminal interface, it is shown as an ordinary result of its own, so give it an `id` and, if it helps, an `action` that leads back. While Claude is being asked for you and your result is on screen, the waiting is said in that lower part too.

```json
{"type": "show", "id": "services-report", "under": "services", "title": "Claude's Report", "kind": "markdown", "text": "…"}
```

`text` is what was typed (at most 1000 characters; empty when there is no field or nothing was typed). Like the search field, the button is said when the result first comes and stays while you send updates. Pressing it is the user's choice, as choosing a command is: it is what lets you then ask Claude, show a form or propose a plan. In the terminal interface the button is Enter in the preview, with a line to type first.

### Lists of files

When your result is a set of files (the changed files of a repository,
duplicates, search hits), don't describe them in text: give Argonaut the
list, and it shows them in the file pane as it shows a folder, with icons,
thumbnails, selection, the right-click menu and your marks and commands.

```json
{"type": "listing", "id": "changes", "title": "Changes on main",
 "folder": "/home/me/project",
 "files": ["/home/me/project/src/main.c", "/home/me/project/notes.txt"],
 "previews": true}
```

Send it again with the same `id` whenever the list changes; an open view
follows. `{"type": "listing", "id": "changes", "remove": true}` takes it away.

| Key | Meaning |
|---|---|
| `id` | Required. Yours to choose; one list per id (up to 20 lists). |
| `title` | Shown in the path bar and the tab. |
| `files` | Full paths, up to 20,000. Files that don't exist are left out of the view. |
| `folder` | The folder the files belong to. Your marks and commands for that folder apply to the files in the list (their paths relative to it), and the view's Location column is relative to it. Argonaut sends you a `watch` for it while the list is shown. |
| `previews` | `true`: for every file in this list you are asked for the preview, whatever its type, with `"listing": "<id>"` in the request. Answer `"kind": "none"` for files you have nothing special to show for. |

To open the list in the pane the user is in (usually as the answer to one of
your commands):

```json
{"type": "show", "kind": "listing", "listing": "changes"}
```

Git's **Show Changes** is exactly this: the list of changed files, and each
file's diff as its preview.

### Places in the sidebar

An extension that brings somewhere new to go (a cloud drive, a kind of
server, a phone) can have rows in the sidebar, in a group of their own at
the end:

```json
{"type": "places", "places": [
    {"id": "work", "label": "Work Drive", "icon": "cloud",
     "folder": "/home/me/.cache/my-drive/work", "eject": true,
     "tooltip": "me@example.com"},
    {"id": "connect", "label": "Connect a Drive…", "icon": "cloud"}
]}
```

Send it whenever your rows change; it replaces what you said before (an
empty list takes them all away).

| Key | Meaning |
|---|---|
| `id` | Required. Comes back when the row is used. |
| `label` | Required. Up to 60 characters. |
| `icon` | `"cloud"`, `"network"`, `"drive"`, `"removable"` or `"folder"` (the default). |
| `folder` | A real folder, by its full path. A click opens it, like any place; files can be dropped on the row. |
| `eject` | `true`: the row has a ⏏ button (and "Disconnect" in its right-click menu). |
| `tooltip` | A line or two shown over the row. |

A row **without** a `folder` is a request to you. When it is clicked, and
when a row's ⏏ is pressed, you are told:

```json
{"type": "place", "id": "connect", "action": "open"}
{"type": "place", "id": "work", "action": "eject"}
```

Do what it takes (sign in, mount, unmount), send `places` again if your rows
changed, and to show the user where they now are:

```json
{"type": "open", "folder": "/home/me/.cache/my-drive/work"}
```

`open` shows a folder in the pane the user is in. It is an **answer**:
Argonaut obeys one `open` after the user chose something of yours (a row, or
one of your commands), within five minutes, and ignores any other. An
extension can't move the user about on its own. At most 20 rows.

### Addresses

Some places aren't folders until something is done: `sftp://me@example.com/`
has to be connected first. Say in your `extension.json` which kinds are
yours:

```json
"addresses": ["sftp", "smb"]
```

When the user types such an address in the path bar (or one is given on the
command line), you are told, and started first if you weren't running:

```json
{"type": "address", "address": "sftp://me@example.com/projects"}
```

Do what it takes, then answer with `open` and the folder. You can't claim
`file`, the web's `http`/`https`, or Argonaut's own places; a kind two
extensions claim goes to the first.

### Forms

When you need the user in the middle of your work (a login, an unknown host
key, which of several things), show a form:

```json
{"type": "form", "id": "login", "title": "Log In", "heading": "Log in to nas",
 "text": "Enter user and password for share “photos”.",
 "warning": "That user name or password didn't work. Try again.",
 "fields": [
     {"id": "anonymous", "kind": "check", "label": "Connect anonymously", "alone": true},
     {"id": "user", "label": "User", "value": "me"},
     {"id": "password", "kind": "secret", "label": "Password"}
 ],
 "buttons": ["Connect"]}
```

Argonaut shows it as a dialog of its own, with Cancel added, and answers:

```json
{"type": "form", "id": "login", "button": 0,
 "values": {"anonymous": "false", "user": "alice", "password": "…"}}
{"type": "form", "id": "login", "cancelled": true}
```

| Key | Meaning |
|---|---|
| `id` | Required. Comes back with the answer. |
| `title`, `heading`, `text` | The dialog's title, a heading and a paragraph. Plain text. |
| `warning` | A line shown as a problem (the last answer was wrong). |
| `fields` | Up to 12, each with an `id` and a `label`. |
| `buttons` | Up to 6 labels; `button` in the answer is the one pressed, counted from 0. None given: "OK". A form with buttons and no fields is a question ("Log In Anyway" / "Cancel Login"). |

A field's `kind`:

| Kind | Shown as | Its value in the answer |
|---|---|---|
| `text` (default) | A line of text starting with `value`, with `placeholder` while empty. With `choices`, a list of suggestions to pick or type over. | The text. |
| `secret` | A password field. It always starts empty. | The text. |
| `check` | A tick box (`"value": true` to start ticked). With `"alone": true`, ticking it turns the other fields off. | `"true"` or `"false"`. |
| `choice` | One of `choices` (up to 200). | The one chosen. |

Rules, because a dialog that asks for a password is a serious thing:

- A form is shown only **while you are doing something the user asked of
  you** (one of your commands, sidebar rows or addresses, within five
  minutes; an answered form keeps that going). Otherwise it is refused: you
  get `"cancelled": true` at once.
- One form at a time; a second while one is open is refused the same way.
- The dialog always says **which extension is asking**. You can't change
  that line.
- What is typed is handed to you and kept nowhere by Argonaut. Don't write
  passwords to disk or to your output.

### Proposing changes

When what you have worked out is a set of changes to a folder (items to
move into sub-folders, better names), don't make them: **propose** them.
Argonaut shows them in the preview pane with a tick each, the user keeps
what they want, and Argonaut makes the changes itself, through its own file
engine, so Ctrl+Z undoes them.

```json
{"type": "plan", "id": "tidy", "title": "Tidy Downloads", "folder": "/home/ann/Downloads",
 "report": "## What is here\nMostly invoices and photos.",
 "note": "Looked at 212 items",
 "moves": [
     {"item": "invoice-march.pdf", "folder": "Invoices/2026", "reason": "An invoice"},
     {"item": "IMG_2041.jpg", "folder": "Photos", "new_name": "Harbour at dusk.jpg", "reason": "A photo"}
 ]}
```

| Key | Meaning |
|---|---|
| `id` | Required. Comes back with the answer. |
| `folder` | Required. The folder the changes are about: an absolute path to a folder that exists. |
| `moves` | Up to 500. Each has an `item` (a **name** in that folder, not a path), and a `folder` to move it into (relative, inside the plan's folder; it is made if it isn't there), or a `new_name`, or both; and a `reason`, one line. |
| `action` in a move | What is done with the item. Left out, or `"move"`: as above. `"copy"`: the same, and the item stays where it is. `"trash"`: to the Trash. `"compress"`: into the archive `new_name` (`.zip`, `.tar.gz`, `.tar.xz` or `.7z`; in `folder` if given), together with every other move that names the same archive; the items stay. `"folder"`: a new, empty `folder` (no `item`). **These are all there are**: a move with any other `action` is left out of the plan, and there is no step that deletes for good or runs anything. Everything but moving and renaming is only done in the user's own folders and drives; an item has one step at most. |
| `title`, `note` | The pane's title, and a line under the moves. |
| `report` | Markdown shown above the moves: what you found. Nothing it names is loaded. |

Argonaut checks every move by its own rules, and shows one it won't make
in red with the reason, unticked: an item that isn't in the folder, a
target outside the folder, a name that isn't a file name, two items to one
place, a place that is taken, a folder into itself. When the user presses
Apply it checks again, because the folder may have changed meanwhile. Then
it tells you:

```json
{"type": "plan", "id": "tidy", "applied": 2, "skipped": 0}
{"type": "plan", "id": "tidy", "dismissed": true}
```

The rules are those of a form: a plan is shown only while you are doing
something the user asked of you, one at a time (a second one, or one out
of the blue, is answered `"dismissed": true` at once). The terminal
interface shows forms and plans too, as boxes over the files, so an
extension that uses them works there without doing anything for it.

### Asking Claude

An extension can have Claude asked something **through Argonaut**, with the
API key and the model the user set in Preferences → Claude, at the user's
cost. Your extension never has the key (it is also taken out of your
environment), so there is nothing for it to keep safe or to leak, and the
user enters it once, for every extension.

Say so in `extension.json`; the Extensions window then shows it, and what
you have cost so far:

```json
"uses": ["claude"]
```

Then send the body of a [Messages request](https://docs.anthropic.com/en/api/messages):

```json
{"type": "claude", "id": "q1",
 "request": {"system": "You name files.", "max_tokens": 2000,
             "messages": [{"role": "user", "content": "A name for a photo of a harbour at dusk"}]}}
```

and the whole response comes back, with its text put together and what it cost:

```json
{"type": "claude", "id": "q1", "response": {"content": […], "stop_reason": "end_turn", "usage": {…}},
 "text": "Harbour at dusk", "stop_reason": "end_turn", "dollars": 0.0031,
 "report_file": "/home/you/.local/share/argonaut/claudeReports/my_extension_report_17.11.26_12.54.31.md"}
{"type": "claude", "id": "q1", "error": {"message": "There is no Claude API key yet: Preferences → Claude", "status": 0}}
```

**Argonaut keeps every answer.** Each answered request is written as a Markdown page in the user's `claudeReports` folder (`~/.local/share/argonaut/claudeReports`), named after your extension and the time: the answer, then everything you sent (the system instructions, the conversation, the tools, the settings; pictures and documents are named with their size, not copied). You don't do anything for this and can't switch it off: it is the user's record of what was asked in their name. `report_file` says where it is, should you want to tell them.

Tools work as in the API: send `tools`, find the `tool_use` blocks in
`response.content`, and send the conversation back with your `tool_result`
in the next request.

Two questions are free and need no user action:

| `"ask"` | You get |
|---|---|
| `"info"` | `hasKey`, the `model` and `modelName` the user chose, and `mode` (`"names"`, `"ask"` or `"auto"`: what the user lets Claude see; respect it). |
| `"count"` (with a `request`) | `tokens`: how many the request is, and `dollars`: what sending them would cost. Tell the user before you spend. |

While a request is under way, Argonaut says so in the preview pane
("Waiting for Claude's answer", with the seconds), so you needn't. The user
can stop waiting there: you then get an `error` for that request at once,
and its answer is dropped when it comes.

What is not yours to decide: the `model` (the user's), `max_tokens` above
16000, streaming. And when: **a request is sent only while you are doing
something the user chose** (a command of yours within the last five
minutes, renewed by each form they answer), at most 12 for one such choice.
An extension in the background can't spend the user's money.

### Previews

Say once, after `hello`, which kinds of files you preview:

```json
{"type": "previews", "types": ["text/markdown", "model/stl", "image/x-xcf"]}
```

Types are matched like a command's `types` (`"image/*"` works), against what
the file really is. When the user previews such a file, you get:

```json
{"type": "preview", "id": 12, "path": "/home/me/notes.md", "mime": "text/markdown",
 "width": 720, "height": 440}
```

`width` and `height` are the pixels there is room for. Answer with the same
`id`, in one of these forms:

```json
{"type": "preview", "id": 12, "kind": "markdown", "text": "# Notes\n\nSome *text*."}
{"type": "preview", "id": 12, "kind": "text", "text": "plain text", "monospace": true}
{"type": "preview", "id": 12, "kind": "html", "text": "<table>…</table>", "note": "3 sheets"}
{"type": "preview", "id": 12, "kind": "diff", "text": "@@ -1 +1 @@\n-old\n+new"}
{"type": "preview", "id": 12, "kind": "image", "image": "/home/me/.cache/my-ext/12.png"}
{"type": "preview", "id": 12, "kind": "none"}
```

| Kind | What Argonaut shows |
|---|---|
| `markdown` | The text, formatted (CommonMark and GitHub tables). |
| `diff` | A unified diff (what `git diff` prints), in a fixed-width font with added lines on green and removed lines on red, in the theme's colours. The text is shown as it is. |
| `text` | The text as it is; `monospace` for a fixed-width font without wrapping. |
| `html` | Qt's rich-text subset: headings, lists, tables, bold, colours. No scripts, and nothing the HTML names is loaded (no pictures, no style sheets). Links are shown but don't open. |
| `image` | A picture file you wrote (PNG, JPEG…), by full path. Argonaut reads it at once and scales it to fit, so you may reuse the file for the next request. With `"scroll": true` beside it, the picture is not scaled: it is shown at its own size and scrolls where it is taller than the pane, for a list you drew (draw it as wide as the `width` a `result` message told you, and as long as the list is, at most 16384 pixels); made again while it is shown, it stays where it was scrolled to. Such a picture can also have **parts with a tooltip** (`"tips"`, below). |
| `none` | "Not from me": Argonaut shows its own preview for the file. |

**Tooltips on a picture.** Beside `"scroll": true`, a `"tips"` list says which parts of the picture have something to say when the mouse rests on them or presses them (the ⓘ beside each service in Running Services):

```json
{"type": "show", "id": "services", "kind": "image", "image": "/run/user/1000/services.png", "scroll": true,
 "tips": [{"x": 148, "y": 214, "width": 26, "height": 26, "text": "D-Bus User Message Bus\ndbus.service · a service of your session"}]}
```

`x`, `y`, `width` and `height` are in the picture's own pixels; `text` is plain text (line breaks are kept, nothing in it is formatting). Argonaut shows it as a tooltip and makes the pointer a hand over the part; you draw the sign yourself. At most 400 parts, 600 characters each; a part without a size or without text is left out. The terminal interface has no tooltips: put there what must be read into the text itself.

`text` may be up to a million characters; `note` is one line under the
preview. You always answer with text or a picture; an extension never draws
in the window itself.

Things to know:

- **Answer within 4 seconds**, or Argonaut gives up on you for that file and
  shows its own preview. An answer for a file the user has already left is
  thrown away, so when several requests are waiting, only the last one is
  worth doing.
- **A file Argonaut has found to be a program in disguise is never sent** to
  an extension.
- **The file comes from anywhere.** If previewing it means parsing a complex
  format, do that in a sandbox (the Document Preview extension starts itself
  again inside bubblewrap for spreadsheets) and never run anything the file
  contains. If you escape what you put into `html`, a cell saying
  `<b>bold</b>` shows as text, as it should.
- If two extensions preview the same type, the first in the Extensions
  window's list is asked.

## A complete example

`word-count.py` marks text files with more than 1,000 words:

```python
#!/usr/bin/env python3
import json, os, sys

def answer(folder):
    badges = {}
    try:
        for entry in os.scandir(folder):
            if entry.is_file() and entry.name.endswith(".txt") and entry.stat().st_size < 5_000_000:
                with open(entry.path, errors="replace") as f:
                    words = len(f.read().split())
                if words > 1000:
                    badges[entry.name] = {"text": "L", "color": "accent",
                                          "tooltip": f"Long text: {words} words"}
    except OSError:
        pass
    info = {"text": f"{len(badges)} long"} if badges else None
    message = {"type": "folder", "folder": folder, "badges": badges}
    if info:
        message["info"] = info
    sys.stdout.write(json.dumps(message) + "\n")
    sys.stdout.flush()

for line in sys.stdin:
    try:
        message = json.loads(line)
    except ValueError:
        continue
    if message.get("type") in ("watch", "refresh"):
        answer(message["folder"])
```

Make it executable (`chmod +x word-count.py`), put it with its
`extension.json` in the folder above, open Edit → Extensions (Ctrl+Shift+X), press Look Again and switch
it on.

A real extension should not block on slow work while messages wait: do slow
work in the background and send the `folder` message when it is ready, as the
Git extension does.
