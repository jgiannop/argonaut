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
            {"id": "", "label": "No id"},
            {"id": "nolabel"}]})
    if m.get("type") == "hello":
        say({"type": "previews", "types": ["text/csv", "image/x-xcf"]})
    if m.get("type") == "preview":
        name = os.path.basename(m.get("path", ""))
        reply = {"type": "preview", "id": m.get("id")}
        if "nothing" in name:
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
    if m.get("type") == "invoke":
        sel = m.get("selection", [])
        say({"type": "message", "text": "invoked %s on %d: %s" % (
            m.get("command"), len(sel), ",".join(os.path.basename(p) for p in sel))})
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
