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
