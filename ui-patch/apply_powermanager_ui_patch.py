#!/usr/bin/env python3
"""
Apply the usermod web-UI injection hook to a WLED source tree.

Since PowerManager v1.1.0 the segment-card "Power relays" menu ships INSIDE the usermod
(served by the device at /um.js). What the WLED tree needs is only this small, usermod-
agnostic hook (design by @blazoncek, proposed to WLED upstream - once it is merged and
you build against a WLED version that includes it, this patch is unnecessary):

  wled00/data/index.js      load /um.js and call umInject(state) after every state render
  wled00/fcn_declare.h      Usermod::addUIInjectCode() virtual + UsermodManager declaration
  wled00/um_manager.cpp     UsermodManager::addUIInjectCode() fan-out
  wled00/wled_server.cpp    the /um.js endpoint

The script locates each edit by structural anchors (not line numbers), is idempotent,
creates one-time .mrbak backups, and only writes a file when every edit in it succeeded.

Usage:
    python apply_powermanager_ui_patch.py [path-to-wled-tree] [--dry-run]

Without a path, WLED trees are auto-discovered in the current and script directory.
After patching, rebuild the firmware - html_ui.h is regenerated from index.js and the
terser minification step doubles as a syntax check of the inserted JS.
"""

import re
import sys
import shutil
from pathlib import Path

# ---------------------------------------------------------------------------------------
# Payloads (kept byte-identical to PowerManager-UI.patch.md)
# ---------------------------------------------------------------------------------------

JS_LOADER = (
    "\n"
    "// load usermod UI inject code (served by the device when usermods provide any);\n"
    "// umInject(state) is then called after every state render, see readState()\n"
    "function loadUmInject() {\n"
    "\tif (gId(\"um\")) return; // already loaded\n"
    "\tlet scE = d.createElement(\"script\");\n"
    "\tscE.id = \"um\";\n"
    "\tscE.src = getURL(\"/um.js\");\n"
    "\tscE.async = false;\n"
    "\tscE.onload = () => {\n"
    "\t\tif (typeof umInject == \"function\") requestJson(); // render once with state available\n"
    "\t};\n"
    "\tscE.onerror = (ev) => {\n"
    "\t\tconsole.log(\"Usermod inject script not present or failed to load\", ev);\n"
    "\t};\n"
    "\td.body.appendChild(scE);\n"
    "}\n"
)
JS_RENDER_HOOK = "\tif (typeof umInject == \"function\") umInject(s); // usermod UI injections (see loadUmInject())\n"
JS_LOAD_HOOK   = "\t\t\tif (json?.info?.u) loadUmInject(); // usermods present: load their UI inject code\n"

H_DEFINE = (
    "// usermods can inject JS into the main web UI via addUIInjectCode() (served at /um.js);\n"
    "// external usermods can test this macro to stay compatible with older WLED bases\n"
    "#define WLED_ENABLE_UM_UI_INJECT\n"
    "\n"
)
H_VIRTUAL = "    virtual void addUIInjectCode(Print &dest) {}                             // print JS code injecting UI elements into the main web UI (served at /um.js, run after every state render)\n"
H_DECL    = "  void addUIInjectCode(Print &dest);\n"

UM_IMPL = "void UsermodManager::addUIInjectCode(Print &dest) { for (auto mod = DYNARRAY_BEGIN(usermods); mod < DYNARRAY_END(usermods); ++mod) (*mod)->addUIInjectCode(dest); } // collect usermod UI inject JS (served at /um.js)\n"

SERVER_ENDPOINT = (
    "\n"
    "  // usermod UI inject code: the main UI loads this script and calls umInject(state) after\n"
    "  // every state render, letting usermods add their own elements without patching index.js\n"
    "  server.on(F(\"/um.js\"), HTTP_GET, [](AsyncWebServerRequest *request) {\n"
    "    AsyncResponseStream *response = request->beginResponseStream(FPSTR(CONTENT_TYPE_JAVASCRIPT));\n"
    "    response->addHeader(FPSTR(s_cache_control), F(\"no-store\"));\n"
    "    response->addHeader(F(\"Expires\"), F(\"0\"));\n"
    "    response->print(F(\"function umInject(s){\"));\n"
    "    UsermodManager::addUIInjectCode(*response);\n"
    "    response->print(F(\"}\"));\n"
    "    request->send(response);\n"
    "  });\n"
)

# Each edit: (name, marker meaning "already applied", anchor regex, payload inserted after the match)
FILES = {
    "wled00/data/index.js": [
        ("loadUmInject fn", "function loadUmInject",
         r"^function getURL\(path\) \{\n\treturn \(loc \? locproto \+ \"//\" \+ locip : \"\"\) \+ path;\n\}\n",
         JS_LOADER),
        ("render hook", "umInject(s);",
         r"^\tupdateUI\(\);\n(?=\treturn true;\n\})",
         JS_RENDER_HOOK),
        ("load hook", "loadUmInject();",
         r"^\t\t\treadState\(s\);\n(?=\n\t\t\treqsLegal = true;)",
         JS_LOAD_HOOK),
    ],
    "wled00/fcn_declare.h": [
        ("capability define", "WLED_ENABLE_UM_UI_INJECT",
         r"^const unsigned int um_data_size = sizeof\(um_data_t\);.*\n\n",
         H_DEFINE),
        ("usermod virtual", "virtual void addUIInjectCode",
         r"^    virtual void onStateChange\(uint8_t mode\) \{\}.*\n",
         H_VIRTUAL),
        ("manager decl", "void addUIInjectCode(Print &dest);",
         r"^  void onStateChange\(uint8_t\);\n",
         H_DECL),
    ],
    "wled00/um_manager.cpp": [
        ("manager impl", "UsermodManager::addUIInjectCode",
         r"^void UsermodManager::onStateChange\(uint8_t mode\).*\n",
         UM_IMPL),
    ],
    "wled00/wled_server.cpp": [
        ("/um.js endpoint", "\"/um.js\"",
         r"^  server\.on\(F\(\"/freeheap\"\), HTTP_GET, \[\]\(AsyncWebServerRequest \*request\)\{\n.*\n  \}\);\n",
         SERVER_ENDPOINT),
    ],
}


def find_tree(arg):
    if arg:
        p = Path(arg)
        if (p / "wled00" / "data" / "index.js").is_file():
            return p
        sys.exit("ERROR: '%s' does not look like a WLED tree (wled00/data/index.js missing)" % arg)
    roots = [Path.cwd(), Path(__file__).resolve().parent]
    found = []
    for root in dict.fromkeys(roots):
        for base in [root] + sorted(x for x in root.iterdir() if x.is_dir()):
            if (base / "wled00" / "data" / "index.js").is_file() and base not in found:
                found.append(base)
    if not found:
        sys.exit("ERROR: no WLED tree found here. Pass the tree root as an argument.")
    if len(found) == 1:
        return found[0]
    print("Multiple WLED trees found:")
    for n, f in enumerate(found, 1):
        print("  %d) %s" % (n, f))
    try:
        return found[int(input("Patch which one? ")) - 1]
    except (ValueError, IndexError, EOFError):
        sys.exit("Aborted.")


def main():
    args = [a for a in sys.argv[1:] if a != "--dry-run"]
    dry = "--dry-run" in sys.argv[1:]
    tree = find_tree(args[0] if args else None)
    print("Tree: %s" % tree)

    applied_total, failed = 0, []
    for rel, edits in FILES.items():
        path = tree / Path(rel)
        if not path.is_file():
            print("  %-34s FAILED (file missing)" % rel)
            failed.append(rel)
            continue
        raw = path.read_text(encoding="utf-8", newline="")
        crlf = "\r\n" in raw
        text = raw.replace("\r\n", "\n") if crlf else raw
        applied_here = 0
        file_ok = True
        for name, marker, anchor, payload in edits:
            label = "%s: %s" % (path.name, name)
            if marker in text:
                print("  %-34s already present" % label)
                continue
            m = re.search(anchor, text, re.M)
            if not m:
                print("  %-34s FAILED (anchor not found)" % label)
                failed.append(label)
                file_ok = False
                continue
            text = text[:m.end()] + payload + text[m.end():]
            applied_here += 1
            print("  %-34s applied" % label)
        if applied_here and file_ok and not dry:
            backup = path.with_suffix(path.suffix + ".mrbak")
            if not backup.exists():
                shutil.copy2(path, backup)
            path.write_text(text.replace("\n", "\r\n") if crlf else text, encoding="utf-8", newline="")
        applied_total += applied_here

    if failed:
        print("\nNOT everything was patched: %d edit(s) did not match this WLED version." % len(failed))
        print("Apply those by hand using PowerManager-UI.patch.md - or, if your WLED base already")
        print("includes the upstream usermod UI injection mechanism, no patch is needed at all.")
        sys.exit(1)
    if applied_total == 0:
        print("\nNothing to do - the hook is already fully applied.")
    elif dry:
        print("\nDry run: %d edit(s) would be applied, nothing written." % applied_total)
    else:
        print("\nOK: %d edit(s) applied. Rebuild the firmware now (html_ui.h regenerates from index.js)." % applied_total)


if __name__ == "__main__":
    main()
