#!/usr/bin/env python3
# How long a file-chooser backend is busy before it can react, measured
# without looking at the screen (so it works on Wayland and X11 alike):
# ask for an Open dialog on FOLDER, ask at once for it to be closed again,
# and time until the backend answers. A dialog flashes on screen each run.
# usage: dialog-bench.py [FOLDER] [RUNS]
import os, sys, time
from gi.repository import Gio, GLib
folder = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/Downloads")
runs = int(sys.argv[2]) if len(sys.argv) > 2 else 4
bus = Gio.bus_get_sync(Gio.BusType.SESSION)
ctx = GLib.MainContext.default()
print("session: %s, folder: %s (%d entries)" % (os.environ.get("XDG_SESSION_TYPE"), folder, len(os.listdir(folder))))
for backend in ("argonaut", "gnome"):
    name = "org.freedesktop.impl.portal.desktop." + backend
    times = []
    for i in range(runs):
        stamp = int(time.time() * 1000)
        handle = "/org/freedesktop/portal/desktop/request/bench/t%d" % stamp
        opts = {"current_folder": GLib.Variant("ay", folder.encode() + b"\0")}
        done = [None]
        def answered(src, res):
            try:
                bus.call_finish(res)
            except Exception:
                pass
            done[0] = time.monotonic()
        t0 = time.monotonic()
        bus.call(name, "/org/freedesktop/portal/desktop", "org.freedesktop.impl.portal.FileChooser", "OpenFile",
                 GLib.Variant("(osssa{sv})", (handle, "org.example.Bench", "", "Bench %d" % stamp, opts)),
                 None, Gio.DBusCallFlags.NONE, 60000, None, answered)
        time.sleep(0.3)  # the request object exists by now
        bus.call(name, handle, "org.freedesktop.impl.portal.Request", "Close", None, None,
                 Gio.DBusCallFlags.NONE, 60000, None, lambda *a: None)
        while done[0] is None and time.monotonic() - t0 < 60:
            ctx.iteration(False)
            time.sleep(0.005)
        times.append(done[0] - t0 if done[0] else None)
        time.sleep(2)
    print("%-9s busy for: %s seconds" % (backend, "  ".join("%.2f" % t if t else "no answer" for t in times)))
