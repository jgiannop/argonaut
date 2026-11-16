#!/usr/bin/env python3
import json, os, sys
with open(os.path.join(os.path.dirname(__file__), "starts"), "a") as f:
    f.write("x")
def say(obj):
    sys.stdout.write(json.dumps(obj) + "\n"); sys.stdout.flush()
for line in sys.stdin:
    try:
        m = json.loads(line)
    except ValueError:
        continue
    folder = m.get("folder", "")
    if m.get("type") == "hello":
        say({"type": "commands", "commands": [
            {"id": "hello", "label": "Fake: Say Hello"},
            {"id": "one-image", "label": "One Image", "kind": "files", "multiple": False, "types": ["image/*"]},
            {"id": "folders", "label": "Folders Only", "kind": "folders"},
            {"id": "here", "label": "Here", "target": "folder"},
            {"id": "ask", "label": "Risky…", "confirm": "Really do the risky thing?"},
            {"id": "typed", "label": "Type & It…", "target": "folder", "group": "Fake/Sub/Too/Deep", "checked": True,
             "ask": {"label": "Say what:", "text": "default"}},
            {"id": "showme", "label": "Show Me", "target": "folder", "group": "Fake"},
            {"id": "listme", "label": "List Me", "target": "folder", "group": "Fake"},
            {"id": "live", "label": "Live", "target": "folder", "group": "Fake"},
            {"id": "late", "label": "Late", "target": "folder", "group": "Fake"},
            {"id": "inmenu", "label": "In The & Menu", "menu": "view"},
            {"id": "nomenu", "label": "In No Menu", "menu": "tools"},
            {"id": "", "label": "No id"},
            {"id": "nolabel"}]})
    if m.get("type") == "hello":
        say({"type": "previews", "types": ["text/csv", "image/x-xcf"]})
        # Sidebar rows: one that is asked when clicked (and can be
        # disconnected), one that is a folder, and some that are wrong.
        here = os.path.dirname(os.path.abspath(__file__))
        say({"type": "places", "places": [
            {"id": "cloud1", "label": "Fake\nCloud", "icon": "cloud", "eject": True, "tooltip": "A pretend drive"},
            {"id": "direct", "label": "Fake Folder", "icon": "skull", "folder": here},
            {"id": "norel", "label": "Not Relative", "folder": "relative/folder"},
            {"id": "cloud1", "label": "Twice"},
            {"id": "", "label": "No id"},
            {"id": "nolabel"}]})
        say({"type": "open", "folder": here})  # out of the blue: not obeyed
    if m.get("type") == "place":
        here = os.path.dirname(os.path.abspath(__file__))
        if m.get("action") == "open":
            say({"type": "open", "folder": "not/absolute"})
            say({"type": "open", "folder": here})
            say({"type": "open", "folder": "/"})  # a second answer to one click: not obeyed
        if m.get("action") == "eject":
            say({"type": "places", "places": [{"id": "direct", "label": "Fake Folder", "folder": here}]})
            say({"type": "message", "text": "ejected " + m.get("id", "")})
    if m.get("type") == "preview":
        name = os.path.basename(m.get("path", ""))
        reply = {"type": "preview", "id": m.get("id")}
        if m.get("listing"):
            reply.update(kind="text", text="LISTING PREVIEW of " + name + " in " + m["listing"])
        elif "nothing" in name:
            reply["kind"] = "none"
        elif "picture" in name:
            import struct, zlib
            def chunk(kind, data):
                body = kind + data
                return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xffffffff)
            rows = b"".join(b"\x00" + b"\xd0\x40\x20" * 8 for _ in range(8))  # 8x8, one colour
            png = os.path.join(os.path.dirname(os.path.abspath(__file__)), "preview.png")
            with open(png, "wb") as f:
                f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 8, 8, 8, 2, 0, 0, 0))
                        + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
            reply.update(kind="image", image=png, note="a picture\nfrom the fake")
        elif "relative" in name:
            reply.update(kind="image", image="preview.png")
        elif "rich" in name:
            reply.update(kind="html", text="<h1>Title</h1><p>Body <img src='/etc/hostname'> text</p>")
        else:
            reply.update(kind="text", text="FAKE PREVIEW of " + name + " as " + m.get("mime", "") , monospace=True)
        say(reply)
    if m.get("type") == "result":
        # A named result is on screen (the answer: a new version made for
        # the room there is) or no longer is. Written down for the tests.
        with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "results"), "a") as f:
            f.write("%s %s\n" % (m.get("id"), "shown" if m.get("shown") else "hidden"))
        if m.get("shown"):
            say({"type": "show", "id": m.get("id"), "update": True, "title": "Live", "kind": "text",
                 "text": "live 2 for %dx%d %s" % (m.get("width", 0), m.get("height", 0), "dark" if m.get("dark") else "light")
                 + " " + ",".join(sorted(m.get("colors", {}))) + " font=" + ("yes" if m.get("font") else "no")})
    if m.get("type") == "invoke":
        sel = m.get("selection", [])
        if m.get("command") == "live":
            say({"type": "show", "id": "live1", "title": "Live", "kind": "text", "text": "live 1"})
        if m.get("command") == "late":
            say({"type": "show", "id": "live1", "update": True, "title": "Live", "kind": "text", "text": "live 3, too late"})
        if m.get("command") == "listme":
            say({"type": "listing", "id": "my list/1", "title": "Fake\nlist", "folder": folder, "previews": True,
                 "files": [folder + "/a.txt", folder + "/missing.txt", "relative.txt", folder + "/sub/../b.txt"]})
            say({"type": "show", "kind": "listing", "listing": "my list/1"})
            say({"type": "show", "kind": "listing", "listing": "no such list"})
        if m.get("command") == "showme":
            say({"type": "show", "title": "A result\nfrom the fake", "kind": "diff", "note": "2 lines",
                 "text": "diff --git a/x b/x\n@@ -1 +1 @@\n-old <b>line</b>\n+new line"})
        say({"type": "message", "text": "invoked %s on %d: %s%s" % (
            m.get("command"), len(sel), ",".join(os.path.basename(p) for p in sel),
            " [" + m["input"] + "]" if m.get("input") else "")})
    if m.get("type") in ("watch", "refresh"):
        if folder.endswith("crash"):
            os._exit(3)
        if folder.endswith("garbage"):
            sys.stdout.write("this is not JSON\n[1, 2]\n{\"type\": \"other\"}\n")
        say({"type": "folder", "folder": folder,
             "info": {"text": "fake " + m["type"] + "\nsecond line", "icon": "skull",
                      "tooltip": "key=" + os.environ.get("ANTHROPIC_API_KEY", "none")},
             "badges": {
                 "a.txt": {"text": "LONG", "color": "accent", "tooltip": "A mark"},
                 "sub": {"text": "", "color": "purple", "deep": True},
                 "../outside.txt": {"text": "!"},
                 "/etc/passwd": {"text": "!"},
                 "x/../../y": {"text": "!"}},
             "commands": [{"id": "only-a", "label": "Only A", "items": ["a.txt", "../outside.txt"]}]})
        say({"type": "folder", "folder": "/not/watched", "badges": {"z": {"text": "Z"}}})
