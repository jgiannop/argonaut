# Argonaut and GNOME's file dialog: a speed comparison

Apps such as Signal, Firefox and Chrome ask the desktop for their Open and
Save windows. On GNOME that window is normally GNOME's own; Argonaut can take
its place (Preferences → Open and Save Windows). On the machine below,
Argonaut's window is ready in about 0.2 seconds whatever the folder, while
GNOME's takes from 0.6 seconds for an almost empty folder to about 7 seconds
for a Downloads folder with 350 entries.

This page gives the measurements, how they were made, and the cause. The
cause is one specific inefficiency in GTK 4 on X11, not GPU drawing and not
slow disk access.

All numbers are from **one machine, in an X11 session, on 15 November 2026**.
They say nothing yet about Wayland, where the cause found here should not
apply (see [What is not known](#what-is-not-known)).

## The machine

| | |
|---|---|
| System | Ubuntu 24.04.5, GNOME Shell 46, **X11 session** |
| CPU, memory | Intel Core i7-3770 (2012), 31 GB |
| Graphics | NVIDIA GeForce RTX 3050, proprietary driver 595.99 |
| Disk | ext4 on an internal drive |
| GNOME's dialog | GTK 4.14.5's built-in file chooser, shown by xdg-desktop-portal-gnome 46.2 |
| Nautilus | 46.4 |
| Argonaut | 0.6.0 (Qt 6.4.2, Qt Widgets) |

The system sets `GSK_RENDERER=gl` for all GTK apps (a file in
`/etc/environment.d`, a common workaround for NVIDIA). Tests 3 and 4 below
also try the other settings.

"GNOME's dialog" here is not Nautilus. In GNOME 46 the portal shows GTK's own
file chooser widget. Nautilus, the file manager, is separate code and was
measured separately (test 6).

## Results in short

Time from the app's request until the dialog shows the folder's files:

| Folder | Argonaut | GNOME's dialog |
|---|---|---|
| 3 files | 0.15 s | 0.65 s |
| Home folder, 151 entries | about 0.2 s | 1.9 to 2.5 s |
| Downloads, 354 entries | 0.2 s | 5 to 7 s |

Time to go away after the user closes it, while it is still loading:

| | Argonaut | GNOME's dialog |
|---|---|---|
| Downloads | 0.02 s | 3.5 to 4.7 s |

GNOME's window exists after about 0.2 seconds, but it stays undrawn until
the times in the first table, and it does not react to anything until then.

## How it was measured

The scripts are in [`tools/bench`](../tools/bench). Each one calls a
file-chooser backend directly over D-Bus, the way `xdg-desktop-portal` does
on behalf of an app, with an Open request for a given folder:

- `org.freedesktop.impl.portal.desktop.gnome` for GNOME's dialog,
- `org.freedesktop.impl.portal.desktop.argonaut` for Argonaut's.

Both backends were already running ("warm") unless a table says otherwise.

`dialog-bench-x11.py` then watches the window:

- **mapped**: `xwininfo` reports the window as viewable.
- **settled**: the window's picture (captured with `xwd`, compared by
  checksum) changed for the last time. The run ends after a quiet period
  with no change.
- **closing**: after the dialog is asked to close, the time until its window
  is gone and until the app receives its answer.

`dialog-bench.py` measures without looking at the screen, so it also works on
Wayland: it sends the Open request, asks 0.3 seconds later for the dialog to
be closed, and times how long the backend needs to answer. A backend that is
busy cannot answer, so this is the time it stays busy. The 0.3 second wait is
part of every number it prints.

Two cautions about the method, both learned the hard way:

- **"Mapped" is not "visible".** GNOME's window is mapped after 0.2 seconds
  but has drawn nothing. A capture taken then shows the desktop behind it.
  Early versions of these tests timed only the mapping and made GNOME's dialog
  look five to thirty times faster than it is.
- **Closing a dialog too early hides its cost in the next run.** A dialog
  that is still busy keeps the backend busy, and the next request waits
  behind it. The scripts wait for the close to finish, then two more seconds.

## The tests

### 1. Until the files are shown

`dialog-bench-x11.py`, quiet period 7 s for GNOME and 3 s for Argonaut,
Downloads folder (354 entries: 87 PNG, 72 PDF, 69 ZIP, folders and others).
Times in seconds; each column is one run.

| | Run 1 | Run 2 |
|---|---|---|
| GNOME: window mapped | 0.22 | 0.22 |
| GNOME: picture changes at | 0.27, then 6.99 | 0.25, then 7.06, 7.11 |
| Argonaut: window mapped | 0.06 | 0.07 |
| Argonaut: picture changes at | 0.09, 0.15, 0.20 | 0.11, 0.15, 0.19 |

Captures of GNOME's window at 1 s and 3 s are byte-for-byte the same and
show only what is behind the window. The capture at 9 s shows the file list.
A capture of Argonaut's window at 0.3 s shows the folder with its thumbnails.

With a folder of 3 files, GNOME's picture settles at 0.54 to 0.71 s and
Argonaut's at 0.10 to 0.19 s.

### 2. Closing

Closed 0.5 s after the picture stopped changing (which for GNOME, as test 1
shows, is long before it is really done), in the way a window's close button
does (`CLOSE=wm`). Seconds until the app has its answer:

| Folder | Argonaut | GNOME's dialog |
|---|---|---|
| 3 files | 0.01, 0.01, 0.01, 0.01 | 0.02, 0.02, 0.02, 0.02 |
| Home, 151 entries | 0.02, 0.01, 0.01 | 1.37, 1.18, 1.20 |
| Downloads, 354 entries | 0.02, 0.01, 0.01, 0.01 | 4.40, 4.37, 3.59, 4.46 |

Closing through the portal's `Request.Close` gives the same picture (GNOME
4.61, 3.53, 4.68, 4.05, 4.66 s on Downloads; Argonaut 0.02 s). When GNOME's
dialog is left alone until its list has appeared, it closes in 0.02 s too:
closing is not slow in itself, the dialog is busy.

### 3. Does GPU drawing cause it? Small folder

GTK 4 draws through the graphics card. To see how much of the delay that
explains, GNOME's backend was restarted with each drawing mode and the
3-file folder was opened four times. Seconds until the picture settled; the
first run of each row is a cold start of the backend.

| GNOME drawing mode | Runs |
|---|---|
| `gl`, older GPU renderer (this system's setting) | 0.68, 0.62, 0.65, 0.73 |
| `cairo`, CPU drawing | 0.73, 0.37, 0.36, 0.74 |
| `ngl`, GTK 4.14's default GPU renderer | 2.01, 0.73, 0.69, 0.73 |
| Argonaut, for comparison | 0.15, 0.14, 0.16, 0.15 |

CPU drawing was about 0.3 s faster in two of four runs and no faster in the
other two.

### 4. Does GPU drawing cause it? Big folder

Same three modes, Downloads folder. Seconds the dialog stayed busy after the
close request (test 2's method):

| GNOME drawing mode | Runs |
|---|---|
| `gl` | 2.81, 4.63, 4.24, 3.92 |
| `cairo` | 4.06, 4.49, 4.09, 4.36 |
| `ngl` | 2.58, 4.35, 4.19, 4.44 |

No difference. Whatever takes the seconds, it is not how the window is
drawn.

### 5. Busy time, without looking at the screen

`dialog-bench.py` (works on Wayland too). Seconds, including the script's own
0.3 s wait:

| Folder | Argonaut | GNOME's dialog |
|---|---|---|
| Home, 151 entries | 0.31, 0.32, 0.32 | 1.93, 2.42, 2.49 |
| Downloads, 354 entries | 0.32, 0.31, 0.32, 0.32 | 5.89, 4.95, 5.82, 5.99 |

### 6. The file managers themselves

`window-bench-x11.py` starts a file manager on a folder and watches its new
window. Nautilus's background service was already running, so its runs are
warm; every Argonaut run starts a new process. Seconds; "drawn" is the last
change of the picture, with a quiet period of 1.5 s, so it includes
thumbnails arriving and, for Nautilus, its opening animation.

| | Window on screen | Picture first changes | Drawn |
|---|---|---|---|
| Argonaut, Downloads | 0.18 to 0.20 | 0.29 to 0.33 | 0.53 to 0.59 |
| Nautilus, Downloads | 0.31 to 0.37 | 0.37 to 0.48 | 1.25 to 1.31 (one run 5.43) |
| Argonaut, 3 files | 0.18 to 0.21 | 0.18 to 0.28 | 0.37 to 0.52 |
| Nautilus, 3 files | 0.26 to 0.35 | 0.36 to 0.41 | 1.35 to 1.73 |

Nautilus takes about as long for 3 files as for 354. It does not have the
dialog's problem; its extra second is the same for any folder.

## The cause

### What GNOME's dialog spends the time on

The backend was run under `strace` for one Open request on Downloads. (The
tracer slows everything down: the busy period was about 13 s instead of 7.)

| What | Calls | Time inside the calls |
|---|---|---|
| File calls: the folder, the thumbnail cache, and (a third of them) re-reading the time zone file (`statx`, `openat`, `newfstatat`, `readlink`…) | about 14,000 | about 0.3 s |
| Messages with the X server (`writev`, `recvmsg`, `poll` on its socket) | about 90,000 | the rest |

During the 7 seconds of an untraced run the process used about 2.2 seconds
of CPU. It is mostly waiting.

Decoding what it sent to the X server:

| X11 request | Times sent |
|---|---|
| `XIGetClientPointer` | 3,143 |
| `GrabServer` | 3,140 |
| `XIQueryPointer` | 3,140 |
| `UngrabServer` | 3,140 |
| `GetInputFocus` (a wait for the server to catch up) | 3,138 |
| every other request together | about 60 |

So the dialog asks the X server **where the mouse pointer is, 3,140 times**,
about nine times per entry in the folder. Each time it locks the whole X
server, asks, waits for the reply, unlocks, and waits once more. One such
exchange takes about 2 milliseconds; 3,140 of them are the 6 to 7 seconds.

### Which code asks

The backend was stopped inside `XIQueryPointer` at three different moments
(the 600th, 1,300th and 2,200th call). The chain of calls was the same each
time:

```
gtk_widget_set_tooltip_text
  gtk_widget_trigger_tooltip_query
    gdk_device_get_surface_at_position
      XIQueryPointer
```

The file list gives its cells a tooltip as it fills each row. In GTK 4,
setting a tooltip text checks at once whether the pointer is over that widget,
in case a tooltip should be showing. On X11 the toolkit does not know where
the pointer is without asking the server, and it asks in the careful,
blocking way described above. The dialog does this for every cell of every
file before it draws its first frame.

### Is that necessary?

No. Nothing about listing 350 files needs the pointer's position thousands
of times. Asking once per frame, or only when the pointer moves, would give
the same tooltips. It is an operation that looks free (setting a text) with
a slow blocking call hidden inside it, called in a loop.

It is also not a general verdict on GTK 4 or on GPU drawing:

- Drawing mode made no difference (tests 3 and 4).
- Nautilus, also GTK 4, opens the same folder without the delay (test 6).
- On Wayland the toolkit is told where the pointer is and has no server to
  ask, so the same code path should cost nothing there. This has not been
  measured.

### Why Argonaut is not affected

Argonaut's Open and Save window is its ordinary file view:

- **The folder is read on another thread**, one `lstat` per file, and the
  first batch is shown after 15 ms. The window never waits for the listing.
- **Rows are painted, not built.** A row is drawn by one delegate when it is
  on screen; there is no widget per cell, so nothing is created, given a
  tooltip or laid out for the 320 files that are not visible.
- **Types, icons and thumbnails are worked out only for visible rows**, and
  thumbnails on a thread pool.
- **Tooltips are produced when the pointer rests on an item**, not in
  advance.
- **Qt Widgets draws with the CPU into a plain buffer**, so a new window has
  no graphics context to set up. This is a small part of the difference
  (test 3 puts GPU setup at 0.3 s at most), not the main one.

The price is that Argonaut has none of what GPU drawing is good at: no
animated transitions, no kinetic scrolling.

## What is not known

- **Wayland.** Everything here is X11. The cause found is specific to X11,
  so GNOME's dialog is probably much faster on Wayland. Run
  `tools/bench/dialog-bench.py ~/Downloads` in a Wayland session to find out.
- **Other machines.** One computer, with NVIDIA's driver. How long one
  pointer query takes depends on the X server and the compositor; elsewhere
  it may be cheaper or dearer than 2 ms.
- **Newer GTK.** Only GTK 4.14.5 was tested. Whether later versions still
  query the pointer when a tooltip is set has not been checked.
- **Why nine queries per entry.** The number suggests rows are filled more
  than once while the folder loads. That was not traced.
- **The remaining half second.** With 3 files GNOME's dialog still needs
  0.65 s against 0.15 s. That is start-up cost of the window and was not
  broken down further.
- **Run counts are small**: two to four runs per case. The differences are
  far larger than the spread, but the exact figures will move.

## Running the tests yourself

```bash
sudo apt install python3-gi x11-utils x11-apps    # xwininfo, xprop, xwd

# Works on X11 and Wayland: how long each backend is busy
tools/bench/dialog-bench.py ~/Downloads 4

# X11 only: watch the windows
QUIET=7 CLOSE=wm tools/bench/dialog-bench-x11.py gnome    ~/Downloads 2
QUIET=3 CLOSE=wm tools/bench/dialog-bench-x11.py argonaut ~/Downloads 2
tools/bench/window-bench-x11.py 4 argonaut ~/Downloads
tools/bench/window-bench-x11.py 4 nautilus --new-window ~/Downloads
```

The Argonaut backend only exists when Argonaut is installed from its package.
Dialogs and windows flash on screen while the tests run. They open and close
windows and change nothing else.

To see the pointer queries yourself, run GNOME's backend under a tracer in
place of the service, open a dialog on a big folder, and count the writes to
the X server's socket:

```bash
systemctl --user stop xdg-desktop-portal-gnome
strace -f -ttt -T -s 200 -o trace.log /usr/libexec/xdg-desktop-portal-gnome
# ... open a file dialog from any app, wait for its list, close it, Ctrl+C ...
systemctl --user start xdg-desktop-portal-gnome
```
