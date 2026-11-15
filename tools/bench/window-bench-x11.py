#!/usr/bin/env python3
# Times a file manager window on X11: starts COMMAND, finds the new top-level
# window, and watches its picture. Prints seconds from start to:
#   window   a new window is on screen
#   drawn    its picture last changed (1.5 s without change ends the run)
# then closes that window like its close button would.
# usage: winbench.py RUNS COMMAND...
import ctypes, hashlib, re, subprocess, sys, time
runs, command = int(sys.argv[1]), sys.argv[2:]
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
    ev.xclient.type = 33; ev.xclient.send_event = 1; ev.xclient.window = window
    ev.xclient.message_type = x11.XInternAtom(dpy, b"WM_PROTOCOLS", 0)
    ev.xclient.format = 32
    ev.xclient.data[0] = x11.XInternAtom(dpy, b"WM_DELETE_WINDOW", 0)
    x11.XSendEvent(dpy, ctypes.c_ulong(window), 0, ctypes.c_long(0), ctypes.byref(ev)); x11.XFlush(dpy)
def clients():
    out = subprocess.run(["xprop", "-root", "_NET_CLIENT_LIST"], capture_output=True, text=True).stdout
    return set(re.findall(r"0x[0-9a-f]+", out))
rows = []
for i in range(runs):
    before = clients()
    t0 = time.monotonic()
    proc = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    wid = None
    while time.monotonic() - t0 < 20 and not wid:
        new = clients() - before
        if new:
            wid = sorted(new)[0]
        else:
            time.sleep(0.003)
    window = time.monotonic() - t0
    last, lastChange, changes = None, window, []
    while wid and time.monotonic() - t0 < 25:
        shot = subprocess.run(["xwd", "-silent", "-id", wid], capture_output=True).stdout
        now = time.monotonic() - t0
        digest = hashlib.md5(shot[4096:]).digest()
        if digest != last:
            last, lastChange = digest, now
            changes.append(now)
        elif now - lastChange > 1.5:
            break
    rows.append((window, lastChange, changes))
    if wid:
        wm_delete(int(wid, 16))
    time.sleep(2.5)
    if proc.poll() is None and "nautilus" not in command[0]:
        proc.terminate()
print("  window  " + " ".join("%.2f" % r[0] for r in rows))
print("  drawn   " + " ".join("%.2f" % r[1] for r in rows))
for r in rows:
    print("    picture changed at: " + " ".join("%.2f" % c for c in r[2][:14]))
