# Writing an Argonaut extension

An extension is a small program that adds to what Argonaut's file views show.
It can be written in any language. Argonaut starts it and talks to it in JSON.
The Git marks (M / A / U / C and the branch in the path bar) are an extension:
its source is in [`extensions/git`](../extensions/git).

Version 1 of the protocol lets an extension:

- put a **mark** on items in a folder: one or two characters, or a dot, in one
  of five colours, with a tooltip;
- show a **short text** for the folder at the right end of the path bar;
- add **commands** to the right-click menu, and say a line to the user when
  one is done.

Three extensions come with Argonaut and are good to read: Git
([`extensions/git`](../extensions/git): marks, the path bar text and commands
that depend on the folder) and Send by Email and Set as Background
([`extensions/desktop`](../extensions/desktop): a command and nothing else, in
about 80 lines).

Extra list columns are planned.

## Safety, for you and for the people who use your extension

An extension is an ordinary program with the user's rights. Argonaut does not
sandbox it. So:

- An extension from the user's own folder **never runs until the user
  switches it on** in Preferences → Extensions.
- Argonaut treats everything an extension sends as data. Texts are cut to
  size, unknown values are replaced, and marks for items outside the folder
  that was asked about are dropped.
- If an extension's folder is removed, it is stopped the next time Argonaut
  looks (when Preferences opens, or at the next start) and its switch is
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
| `name`, `description`, `version`, `author` | Shown in Preferences. |

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
{"type": "hello", "protocol": 1, "app": "argonaut", "version": "0.6.0"}
{"type": "watch", "folder": "/home/me/Documents"}
{"type": "refresh", "folder": "/home/me/Documents"}
{"type": "unwatch", "folder": "/home/me/Documents"}
{"type": "invoke", "command": "mark-done", "folder": "/home/me/Documents",
 "selection": ["/home/me/Documents/report.txt"]}
```

- `watch`: a view now shows this folder. Answer with a `folder` message, now
  or when you know; send another whenever your answer changes.
- `refresh`: something in it may have changed (a file was saved, a copy
  finished, the window came back to the front). Look again; answer if
  anything is different.
- `unwatch`: no view shows it any more. Forget it.
- `invoke`: the user chose one of your commands (and said yes to its
  question, if it has one). `selection` holds the full paths of the selected
  items; it is empty for a command on the folder itself. `folder` is the open
  folder, or `""` in Recent and Starred.

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

If several extensions mark the same item, the first one in Preferences' list
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
| `kind` | `"files"`, `"folders"` or `"any"` (default): what every selected item must be. |
| `multiple` | `false`: only when exactly one item is selected. Default `true`. |
| `types` | Every selected item's type must match one of these: `"image/*"`, or a type such as `"text/plain"` (types derived from it match too). The type is what the file really is, by its content, not only its name. |
| `items` | Only in a `folder` message: the command is offered only when every selected item is in this list of relative paths. |
| `confirm` | A question Argonaut asks first, with Cancel as the default. Without a yes you get no `invoke`. |

At most 20 commands everywhere and 20 per folder. Commands aren't offered in
the Trash or in other programs' Open and Save windows.

To tell the user how it went, at any time:

```json
{"type": "message", "text": "3 pictures made smaller"}
```

It is shown for a few seconds at the bottom of the window (up to 200
characters, one line). Use it for results and errors, not for progress.

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
`extension.json` in the folder above, open Preferences → Extensions and switch
it on.

A real extension should not block on slow work while messages wait: do slow
work in the background and send the `folder` message when it is ready, as the
Git extension does.
