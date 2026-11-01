#!/usr/bin/env python3
"""Check release/user-files.json: the manifest part the desktop installer's "Your ROMs and files" step reads (mpc-vst-plugins PR #668).
1. structure: tools/user_files.py of mpc-vst-plugins (MPC_VST, default ../../mpc-vst-plugins) accepts it for --user-data Content/ROMs;
2. behaviour: a mirror of the installer's matching (extension, size, header; a file passes when any rule does) accepts the real ROMs,
   each under its own rule, and refuses a wrong size, a wrong start and a non-ROM extension.
Usage: test/user_files_check.py [ROM_DIR]   (ROM_DIR = a folder with your v4.4 ROM1 to ROM5; skipped when absent)"""
import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
groups = json.load(open(os.path.join(ROOT, "release", "user-files.json")))
fails = 0


def check(ok, msg):
    global fails
    print(("ok   " if ok else "FAIL ") + msg)
    fails += 0 if ok else 1


mv = os.environ.get("MPC_VST", os.path.join(ROOT, "..", "mpc-vst-plugins"))
sys.path.insert(0, os.path.join(mv, "tools"))
try:
    from user_files import user_files_problems
    p = user_files_problems(groups, ["Content/ROMs"])
    check(not p, "manifest accepted by user_files.py %s" % (p or ""))
except ImportError:
    print("skip structure check: no tools/user_files.py in %s (PR #668 not merged there)" % mv)


def verdict(g, name, data):
    """The installer's matching (tools/desktop/userfiles.go validate): the first rule whose extension, size and header hold."""
    matched = False
    for r in g["rules"]:
        if not any(name.lower().endswith(e.lower()) for e in r["ext"]):
            continue
        matched = True
        if r.get("size") and len(data) != r["size"]:
            continue
        if r.get("header_hex") and not data.startswith(bytes.fromhex(r["header_hex"])):
            continue
        return ("maybe" if r.get("unverified") else "ok"), r["label"]
    return ("bad" if matched else "ignored"), ""


g = groups[0]
check(g["path"] == "Content/ROMs", "folder is Content/ROMs (the plugin looks there)")
check(len({r["header_hex"] for r in g["rules"]}) == 5, "five distinct chip rules")
check(verdict(g, "x.bin", bytes(2048))[0] == "bad", "a 2048-byte file with the wrong start is refused")
check(verdict(g, "x.bin", bytes(4096))[0] == "bad", "a wrong size is refused")
check(verdict(g, "notes.txt", bytes(2048))[0] == "ignored", "a non-ROM extension is ignored")
if len(sys.argv) > 1 and os.path.isdir(sys.argv[1]):
    seen = []
    for i in range(1, 6):
        n = "ROM%d 2716.BIN" % i
        s, label = verdict(g, n, open(os.path.join(sys.argv[1], n), "rb").read())
        check(s == "maybe" and label.startswith("ROM%d " % i), "%s -> %s (%s)" % (n, label, s))
        seen.append(label)
else:
    print("skip real-ROM check: pass a ROM folder")
print("FAILED" if fails else "PASSED")
sys.exit(1 if fails else 0)
