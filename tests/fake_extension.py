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
                 "x/../../y": {"text": "!"}}})
        say({"type": "folder", "folder": "/not/watched", "badges": {"z": {"text": "Z"}}})
