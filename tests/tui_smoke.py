#!/usr/bin/env python3
"""Starts the real terminal interface in a pseudo-terminal, presses a few
keys and F10, and checks it drew its screen and left cleanly. This is the
only test of the ncurses layer; everything else is tested without a terminal
(tst_tui). usage: tui_smoke.py PROGRAM [ARGS...]"""
import fcntl, os, pty, select, shutil, struct, sys, tempfile, termios, time

home = tempfile.mkdtemp(prefix="argonaut-tui-smoke-")
os.makedirs(os.path.join(home, "folder", "inside"))
open(os.path.join(home, "folder", "a-file.txt"), "w").write("x")
open(os.path.join(home, "folder", "inside", "deep-file.txt"), "w").write("x")
pid, fd = pty.fork()
if pid == 0:
    os.environ.update(TERM="xterm-256color", LANG="C.UTF-8", HOME=home, XDG_DATA_HOME=home + "/data",
                      XDG_CONFIG_HOME=home + "/config", XDG_CACHE_HOME=home + "/cache",
                      ARGONAUT_TRASH_HOME_ONLY="1", ARGONAUT_EXTENSIONS_PATH="", ARGONAUT_KEYSTORE="file")
    os.execv(sys.argv[1], sys.argv[1:] + [os.path.join(home, "folder"), home])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 110, 0, 0))
out = b""
def pump(seconds):
    global out
    end = time.time() + seconds
    while time.time() < end:
        ready, _, _ = select.select([fd], [], [], 0.05)
        if ready:
            try:
                data = os.read(fd, 65536)
            except OSError:
                return
            if not data:
                return
            out += data
pump(2.0)
os.write(fd, b"\x1b[B")      # Down
pump(0.4)
os.write(fd, b"\x1bOP")      # F1: the help
pump(0.6)
os.write(fd, b"x")            # any key closes it
pump(0.4)
# A double click on the first item ("inside/": screen row 5, under "..", column 6): into the folder.
for _ in range(2):
    os.write(fd, b"\x1b[<0;6;5M\x1b[<0;6;5m")
    pump(0.15)
pump(0.6)
os.write(fd, b"\x1b[21~")    # F10
status = None
for _ in range(60):
    pump(0.1)
    done, code = os.waitpid(pid, os.WNOHANG)
    if done:
        status = code
        break
text = out.decode("utf-8", "replace")
problems = []
if status is None:
    os.kill(pid, 9)
    problems.append("it did not quit on F10")
elif status != 0:
    problems.append("exit status %d" % status)
for needle in ("Argonaut", "a-file.txt", "inside/", "F10", "copy / move to the other pane", "deep-file.txt"):
    if needle not in text:
        problems.append("never drew %r" % needle)
if "\x1b[?1049l" not in text and "\x1b[?47l" not in text:
    problems.append("did not give the terminal back (no leave-alternate-screen)")
shutil.rmtree(home, ignore_errors=True)
print("\n".join(problems) if problems else "ok")
sys.exit(1 if problems else 0)
