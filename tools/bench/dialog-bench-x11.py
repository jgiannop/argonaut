#!/usr/bin/env python3
# Times a file-chooser backend on X11 by watching its window. For each run,
# seconds from the app's request to:
#   mapped   the window exists on screen (it may still be undrawn)
#   content  the first captured picture that isn't one flat colour (an
#            undrawn window shows what is behind it, so read this with care)
#   settled  the last change of its picture; QUIET seconds without change end
#            the run (default 0.5; use 7 for GNOME's dialog on a big folder)
# and, after asking it to close, until the window is gone and the app has
# its answer. CLOSE=wm closes like the window's close button; otherwise the
# portal's Request.Close is used.
# Needs python3-gi, xwininfo and xwd. Dialogs flash on screen.
# usage: [CLOSE=wm] [QUIET=7] dialog-bench-x11.py gnome|argonaut FOLDER RUNS
import ctypes, hashlib, os, subprocess, sys, time
from gi.repository import Gio, GLib
backend, folder, runs = sys.argv[1], sys.argv[2], int(sys.argv[3])
name = "org.freedesktop.impl.portal.desktop." + backend
bus = Gio.bus_get_sync(Gio.BusType.SESSION)
ctx = GLib.MainContext.default()
rows = []
answered = [None]
def got_answer(src, res):
    try:
        bus.call_finish(res)
    except Exception:
        pass
    answered[0] = time.monotonic()
x11 = ctypes.CDLL("libX11.so.6")
x11.XOpenDisplay.restype = ctypes.c_void_p
x11.XInternAtom.restype = ctypes.c_ulong
class ClientMessage(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int), ("serial", ctypes.c_ulong), ("send_event", ctypes.c_int),
                ("display", ctypes.c_void_p), ("window", ctypes.c_ulong), ("message_type", ctypes.c_ulong),
                ("format", ctypes.c_int), ("data", ctypes.c_long * 5)]
class XEvent(ctypes.Union):
    _fields_ = [("xclient", ClientMessage), ("pad", ctypes.c_long * 24)]
dpy = ctypes.c_void_p(x11.XOpenDisplay(None))
def wm_delete(window):
    ev = XEvent()
    ev.xclient.type = 33
    ev.xclient.send_event = 1
    ev.xclient.window = window
    ev.xclient.message_type = x11.XInternAtom(dpy, b"WM_PROTOCOLS", 0)
    ev.xclient.format = 32
    ev.xclient.data[0] = x11.XInternAtom(dpy, b"WM_DELETE_WINDOW", 0)
    ev.xclient.data[1] = 0
    x11.XSendEvent(dpy, ctypes.c_ulong(window), 0, ctypes.c_long(0), ctypes.byref(ev))
    x11.XFlush(dpy)
for i in range(runs):
    stamp = int(time.time() * 1000)
    title = "Bench %s %d" % (backend, stamp)
    handle = "/org/freedesktop/portal/desktop/request/bench/t%d" % stamp
    opts = {"current_folder": GLib.Variant("ay", folder.encode() + b"\0"), "multiple": GLib.Variant("b", True)}
    t0 = time.monotonic()
    bus.call(name, "/org/freedesktop/portal/desktop", "org.freedesktop.impl.portal.FileChooser", "OpenFile",
             GLib.Variant("(osssa{sv})", (handle, "org.example.Bench", "", title, opts)),
             None, Gio.DBusCallFlags.NONE, 60000, None, got_answer)
    mapped = content = settled = None
    wid = None
    while time.monotonic() - t0 < 20:
        while ctx.iteration(False):
            pass
        out = subprocess.run(["xwininfo", "-name", title], capture_output=True, text=True).stdout
        if "IsViewable" in out:
            mapped = time.monotonic() - t0
            wid = out.split("Window id: ")[1].split()[0]
            break
        time.sleep(0.003)
    last = None
    lastChange = mapped
    changes = []
    while wid and time.monotonic() - t0 < 14:
        while ctx.iteration(False):
            pass
        shot = subprocess.run(["xwd", "-silent", "-id", wid], capture_output=True).stdout
        now = time.monotonic() - t0
        body = shot[4096:]
        if content is None and len(body) > 4000:
            step = max(4, (len(body) // 4 // 20000) * 4)
            if len({body[k:k + 4] for k in range(0, len(body) - 4, step)}) > 40:
                content = now
        digest = hashlib.md5(body).digest()
        if digest != last:
            last, lastChange = digest, now
            changes.append(now)
        elif now - lastChange > float(os.environ.get("QUIET", "0.5")):
            break
    settled = lastChange
    print('  picture changed at:', ' '.join('%.2f' % c for c in changes), flush=True)
    c0 = time.monotonic()
    replied = [None]
    if os.environ.get("CLOSE") == "wm":
        # What the window's close button does: WM_DELETE_WINDOW to that window.
        # The app's answer is the OpenFile reply itself.
        wm_delete(int(wid, 16))
        answered[0] = None
        watch = True
    else:
        watch = False
        def closed(src, res):
            try:
                bus.call_finish(res)
            except Exception:
                pass
            replied[0] = time.monotonic() - c0
        bus.call(name, handle, "org.freedesktop.impl.portal.Request", "Close", None, None,
                 Gio.DBusCallFlags.NONE, 15000, None, closed)
    gone = None
    while time.monotonic() - c0 < 15:
        while ctx.iteration(False):
            pass
        if gone is None and "IsViewable" not in subprocess.run(["xwininfo", "-name", title], capture_output=True, text=True).stdout:
            gone = time.monotonic() - c0
        if watch and replied[0] is None and answered[0] is not None:
            replied[0] = answered[0] - c0
        if gone is not None and replied[0] is not None:
            break
        time.sleep(0.01)
    rows.append((mapped, content, settled, gone, replied[0]))
    time.sleep(2.0)  # the backend is idle again before the next request
f = lambda v: "%.2f" % v if v is not None else " -- "
for label, k in (("mapped ", 0), ("content", 1), ("settled", 2), ("closing: window gone", 3), ("closing: app has its answer" if os.environ.get("CLOSE") == "wm" else "closing: reply", 4)):
    print("  %s  %s" % (label, " ".join(f(r[k]) for r in rows)), flush=True)
