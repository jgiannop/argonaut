#!/usr/bin/env python3
import json, os, sys
with open(os.path.join(os.path.dirname(__file__), "starts"), "a") as f:
    f.write("x")
interface = "?"
# What it would like Claude to be asked: the model and the size are not its to choose.
claude_request = {"model": "some-other-model", "max_tokens": 999999, "stream": True,
                  "system": "Be brief.", "messages": [{"role": "user", "content": "Say hello"}]}
def say(obj):
    sys.stdout.write(json.dumps(obj) + "\n"); sys.stdout.flush()
for line in sys.stdin:
    try:
        m = json.loads(line)
    except ValueError:
        continue
    folder = m.get("folder", "")
    if m.get("type") == "hello":
        interface = m.get("interface", "?")
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
            {"id": "form", "label": "Form", "target": "folder", "group": "Fake"},
            {"id": "late", "label": "Late", "target": "folder", "group": "Fake"},
            {"id": "inmenu", "label": "In The & Menu", "menu": "view"},
            {"id": "nomenu", "label": "In No Menu", "menu": "tools"},
            {"id": "", "label": "No id"},
            {"id": "nolabel"}]})
    if m.get("type") == "hello":
        say({"type": "previews", "types": ["text/csv", "image/x-xcf"]})
    if m.get("type") == "hello" and os.environ.get("ARGONAUT_EXTENSION_ID") != "lazy":
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
        say({"type": "form", "id": "sneaky", "title": "Your password, please",
             "fields": [{"id": "p", "label": "Password", "kind": "secret"}]})  # nor is this shown
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
    if m.get("type") == "address":
        # A kind of address that is ours (the manifest says so).
        say({"type": "message", "text": "address " + m.get("address", "")})
        say({"type": "open", "folder": os.path.dirname(os.path.abspath(__file__))})
    if m.get("type") == "form":
        # The answer to a form of ours (or the refusal to show one).
        with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "forms"), "a") as f:
            f.write(json.dumps(m, sort_keys=True) + "\n")
        if m.get("id") == "login" and not m.get("cancelled"):
            v = m.get("values", {})
            say({"type": "message", "text": "form %s|%s|%s|%s|%s button %d" % (
                v.get("user"), v.get("password"), v.get("anon"), v.get("share"), v.get("where"), m.get("button", -1))})
            say({"type": "form", "id": "again", "text": "Are you sure?", "buttons": ["Yes", "No & Never"]})
    if m.get("type") == "result":
        # A named result is on screen (the answer: a new version made for
        # the room there is) or no longer is. Written down for the tests.
        with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "results"), "a") as f:
            f.write("%s %s\n" % (m.get("id"), "shown" if m.get("shown") else "hidden"))
        if m.get("shown") and m.get("id") == "t1":
            # In the terminal interface: text with colours, for the room in characters.
            say({"type": "show", "id": "t1", "update": True, "title": "In Menu", "kind": "ansi", "note": "made again",
                 "text": "\x1b[32mupdated\x1b[0m cols=%d rows=%d\x1b[2J\x1b]0;title\x07 \x1b[1;38;5;196mhot\x1b[0m\n"
                         % (m.get("columns", 0), m.get("rows", 0)) + "\n".join("row %d" % i for i in range(60))})
        elif m.get("shown"):
            say({"type": "show", "id": m.get("id"), "update": True, "title": "Live", "kind": "text",
                 "text": "live 2 for %dx%d %s" % (m.get("width", 0), m.get("height", 0), "dark" if m.get("dark") else "light")
                 + " " + ",".join(sorted(m.get("colors", {}))) + " font=" + ("yes" if m.get("font") else "no")})
    if m.get("type") == "plan":  # what became of the changes it proposed
        if m.get("dismissed"):
            say({"type": "message", "text": "plan %s dismissed" % m.get("id")})
        else:
            say({"type": "message", "text": "plan %s applied=%d skipped=%d" % (m.get("id"), m.get("applied", 0), m.get("skipped", 0))})
    if m.get("type") == "claude":  # Argonaut's answer to a question for Claude
        if "error" in m:
            say({"type": "message", "text": "claude %s error: %s" % (m.get("id"), m["error"].get("message"))})
        elif "hasKey" in m:
            say({"type": "message", "text": "claude info key=%s model=%s mode=%s" % (m["hasKey"], m.get("model"), m.get("mode"))})
            say({"type": "claude", "id": "count1", "ask": "count", "request": claude_request})
        elif "tokens" in m:
            say({"type": "message", "text": "claude count %d" % m["tokens"]})
            say({"type": "claude", "id": "c1", "request": claude_request})
        else:
            say({"type": "message", "text": "claude said: %s (%s) $%.4f" % (m.get("text"), m.get("stop_reason"), m.get("dollars", 0))})
    if m.get("type") == "search":  # typed in the field over a result that can be searched
        say({"type": "show", "id": m.get("id"), "update": True, "title": "Live", "kind": "text",
             "text": "searched: " + m.get("text", "")})
    if m.get("type") == "watch" and folder.endswith("sneaky"):  # nobody chose anything: Claude isn't asked
        say({"type": "claude", "id": "s1", "request": claude_request})
        say({"type": "plan", "id": "s2", "folder": folder, "moves": [{"item": "x", "folder": "y"}]})  # nor a plan shown
    if m.get("type") == "invoke":
        sel = m.get("selection", [])
        if any(os.path.basename(p) == "make-plan.txt" for p in sel):
            # Changes for the folder: some that can be done, and some that are not its to ask for.
            say({"type": "plan", "id": "p1", "title": "Tidy\nUp", "folder": folder, "note": "No charge",
                 "report": "# Found\n\nThree **texts**. ![x](file:///etc/passwd)",
                 "moves": [
                     {"item": "a.txt", "folder": "Texts", "reason": "A text"},
                     {"item": "b.txt", "folder": "Texts/Old", "reason": "An\nold text"},
                     {"item": "c.txt", "new_name": "renamed.txt", "reason": "A better name"},
                     {"item": "missing.txt", "folder": "Texts", "reason": "Not there"},
                     {"item": "d.txt", "folder": "../outside", "reason": "Out of the folder"},
                     {"item": "../make-plan.txt", "folder": "Texts", "reason": "A path, not a name"},
                     {"item": "e.txt", "new_name": "a.txt", "reason": "Onto another"},
                     {"reason": "no item"}]})
            say({"type": "plan", "id": "p2", "folder": folder, "moves": []})  # one at a time
        if any(os.path.basename(p) == "ask-many.txt" for p in sel):  # more questions than one choice of the user's allows
            for n in range(15):
                say({"type": "claude", "id": "many%d" % n, "request": claude_request})
        if any(os.path.basename(p) == "many-moves.txt" for p in sel):  # more moves than a plan may have
            say({"type": "plan", "id": "big", "folder": folder,
                 "moves": [{"item": "f%d.txt" % n, "folder": "Sorted"} for n in range(700)]})
        if any(os.path.basename(p) == "ask-claude.txt" for p in sel):
            say({"type": "claude", "id": "i1", "ask": "info"})
        if m.get("command") == "form":
            say({"type": "form", "id": "login", "title": "Log\nIn", "heading": "Log in to nas", "text": "Enter <b>it</b>",
                 "warning": "That didn't work", "buttons": ["Connect", "Later", ""],
                 "fields": [
                     {"id": "anon", "kind": "check", "label": "Anonymously", "alone": True},
                     {"id": "user", "label": "User", "value": "me"},
                     {"id": "password", "label": "Password", "kind": "secret", "value": "never shown"},
                     {"id": "share", "kind": "choice", "label": "Share", "choices": ["photos", "music"], "value": "music"},
                     {"id": "where", "label": "Where", "choices": ["here", "there"], "placeholder": "somewhere"},
                     {"id": "empty", "kind": "choice", "label": "Nothing to choose"},
                     {"id": "user", "label": "Twice"},
                     {"label": "No id"}]})
            say({"type": "form", "id": "second", "text": "one at a time"})
        if m.get("command") == "inmenu" and os.environ.get("FAKE_SHOWS_IN_MENU"):
            say({"type": "show", "id": "t1", "title": "In Menu", "kind": "ansi", "note": "interface=" + interface,
                 "text": "plain \x1b[31mred\x1b[0m"})
        if m.get("command") == "live":
            say({"type": "show", "id": "live1", "title": "Live", "kind": "text", "text": "live 1", "search": "Search\nthe fake"})
        if m.get("command") == "late":
            say({"type": "show", "id": "live1", "update": True, "title": "Live", "kind": "text", "text": "live 3, too late"})
        if m.get("command") == "listme":
            say({"type": "listing", "id": "my list/1", "title": "Fake\nlist", "folder": folder, "previews": True,
                 "files": [folder + "/a.txt", folder + "/missing.txt", "relative.txt", folder + "/sub/../b.txt"]})
            say({"type": "show", "kind": "listing", "listing": "my list/1"})
            say({"type": "show", "kind": "listing", "listing": "no such list"})
        if any(os.path.basename(p) == "tips.png" for p in sel):  # a picture with parts that have tooltips
            picture = [p for p in sel if os.path.basename(p) == "tips.png"][0]
            say({"type": "show", "id": "tipped", "title": "Tipped", "kind": "image", "image": picture, "scroll": True,
                 "tips": [
                     {"x": 10, "y": 20, "width": 30, "height": 30, "text": "First <b>tip</b>\nsecond line\x1b[31m"},
                     {"x": 10, "y": 200, "width": 30, "height": 30, "text": "x" * 5000},
                     {"x": -5, "y": 20, "width": 30, "height": 30, "text": "left of the picture"},
                     {"x": 10, "y": 20, "width": 0, "height": 30, "text": "no width"},
                     {"x": 10, "y": 20, "width": 30, "height": 30, "text": "   "},
                     {"x": 10, "y": 900000, "width": 30, "height": 30, "text": "far below"},
                     {"x": "ten", "y": 20, "width": 30, "height": 30, "text": "not a number"},
                     "not an object"] + [{"x": 100, "y": n, "width": 5, "height": 1, "text": "many %d" % n} for n in range(600)]})
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
