#!/usr/bin/env python3
"""
Apply the PowerManager "Power relays" segment-card patch to WLED's wled00/data/index.js.

The four edits are documented in PowerManager-UI.patch.md. This script
locates them by structural anchors (function names / template landmarks), not line
numbers, so it survives unrelated changes to the file. It is idempotent: already
applied edits are detected and skipped. The file is only written when every edit is
either applied or already present; a .mrbak backup is created next to it first.

Usage:
    python apply_powermanager_ui_patch.py [path-to-wled-tree-or-index.js] [--dry-run]

Without a path, WLED trees are auto-discovered in the current and script directory.
After patching, rebuild the firmware - html_ui.h is regenerated from index.js and the
terser minification step doubles as a syntax check of the inserted code.
"""

import re
import sys
import shutil
from pathlib import Path

# ----------------------------------------------------------------------------------
# Payloads (kept byte-identical to PowerManager-UI.patch.md)
# ----------------------------------------------------------------------------------

# Edit 1: relay list extraction, inserted directly after the counter-reset line at the
# top of populateSegments()
P1 = (
    "\n"
    "\t// Power Manager usermod: relays that can be coupled to a segment (row is hidden when the usermod is absent)\n"
    "\tlet mrRelays = [];\n"
    "\tif (s.PowerManager) mrRelays = (Array.isArray(s.PowerManager.relays) ? s.PowerManager.relays : [s.PowerManager])\n"
    "\t\t.filter(r => r.seg !== undefined && r.seg != 99); // 99 = \"any segment\" mode, managed in usermod settings\n"
)

# Edit 2: the collapsible "Power relays" menu, inserted inside the segment loop,
# directly before the card template (the `cn += \`<div class="seg lstI...` line)
P2 = (
    "\t\tlet mrRow = \"\";\n"
    "\t\tif (mrRelays.length) {\n"
    "\t\t\t// keep the relay list open across re-renders (each link click re-renders the cards from state)\n"
    "\t\t\tlet rlyOpen = gId(`seg${i}rlyl`) ? !gId(`seg${i}rlyl`).classList.contains('hide') : false;\n"
    "\t\t\tlet lnk = mrRelays.filter(r=>r.seg==i).map(r=>r.name?r.name:\"Relay \"+r.relay).join(\", \");\n"
    "\t\t\tmrRow = `<div class=\"check revchkl\" style=\"cursor:pointer;\" title=\"Link power relays: their output is cut when this segment is off\" `+\n"
    "\t\t\t\t\t\t`onclick=\"gId('seg${i}rlyl').classList.toggle('hide');gId('seg${i}rlyc').classList.toggle('exp');\">`+\n"
    "\t\t\t\t\t`Power relays: ${lnk?lnk:\"none\"}`+\n"
    "\t\t\t\t\t`<i class=\"icons e-icon${rlyOpen?\" exp\":\"\"}\" id=\"seg${i}rlyc\" style=\"position:absolute;left:0;top:3px;transition:transform .3s;\">&#xe395;</i></div>`+\n"
    "\t\t\t\t\t`<div id=\"seg${i}rlyl\" class=\"${rlyOpen?'':'hide'}\" style=\"margin-left:16px;\">`;\n"
    "\t\t\tfor (const r of mrRelays) {\n"
    "\t\t\t\t// hint where the relay is currently linked: this segment (accent) or another one (dimmed)\n"
    "\t\t\t\tlet other = \"\";\n"
    "\t\t\t\tif (r.seg == i) {\n"
    "\t\t\t\t\tother = ` <span style=\"color:var(--c-g);font-size:smaller;\">(this segment)</span>`;\n"
    "\t\t\t\t} else if (r.seg >= 0) {\n"
    "\t\t\t\t\tlet os = (s.seg||[]).find(q => q.id == r.seg);\n"
    "\t\t\t\t\tother = ` <span style=\"color:var(--c-d);font-size:smaller;\">(${os&&os.n ? os.n : \"Segment \"+r.seg})</span>`;\n"
    "\t\t\t\t}\n"
    "\t\t\t\tmrRow += `<label class=\"check revchkl\">${r.name?r.name:\"Relay \"+r.relay}${other}`+\n"
    "\t\t\t\t\t\t\t`<input type=\"checkbox\" id=\"seg${i}rly${r.relay}\" onchange=\"setSegRly(${i},${r.relay})\" ${r.seg==i?\"checked\":\"\"}>`+\n"
    "\t\t\t\t\t\t\t`<span class=\"checkmark\"></span></label>`;\n"
    "\t\t\t}\n"
    "\t\t\tmrRow += `</div>`;\n"
    "\t\t}\n"
)

# Edit 3: single line referencing the menu inside the card template, inserted directly
# above the `<div class="del">` template line (repeat/delete buttons)
EDIT3_LINE = "\t\t\t\t\tmrRow +\n"

# Edit 4: the click handler, inserted after setSegBri()
P4 = (
    "\n"
    "// Power Manager usermod: (un)couple relay r to/from segment s (power cutoff follows segment on/off)\n"
    "function setSegRly(s, r)\n"
    "{\n"
    "\tvar lnk = gId(`seg${s}rly${r}`).checked;\n"
    "\tvar obj = {\"PowerManager\": {\"relay\": r, \"seg\": lnk ? s : -1}};\n"
    "\trequestJson(obj);\n"
    "}\n"
)

# ----------------------------------------------------------------------------------


def func_span(text, name):
    """Return (start, end) of the top-level function `name` (end = start of the next
    top-level function, or end of file)."""
    m = re.search(r"^function %s\b" % re.escape(name), text, re.M)
    if not m:
        return None
    nxt = re.search(r"^function ", text[m.end():], re.M)
    end = m.end() + nxt.start() if nxt else len(text)
    return (m.start(), end)


def apply_patch(text):
    """Return (new_text, results) where results maps edit name -> status string.
    Statuses: 'applied', 'already present', 'FAILED (<reason>)'."""
    results = {}

    # --- Edit 1: relay list at the top of populateSegments() -----------------------
    if "let mrRelays" in text:
        results["1 relay list"] = "already present"
    else:
        m = re.search(
            r"^[ \t]*segCount\s*=\s*0;\s*lowestUnused\s*=\s*0;\s*lSeg\s*=\s*0;[ \t]*\n",
            text, re.M)
        if m:
            text = text[:m.end()] + P1 + text[m.end():]
            results["1 relay list"] = "applied"
        else:
            results["1 relay list"] = "FAILED (counter-reset line in populateSegments() not found)"

    # --- Edits 2 & 3 operate inside populateSegments() -----------------------------
    span = func_span(text, "populateSegments")
    if span is None:
        for k in ("2 relay menu", "3 template line"):
            results[k] = "FAILED (function populateSegments() not found)"
    else:
        start, end = span
        body = text[start:end]

        # Edit 2: menu builder before the card template
        if "let mrRow" in body:
            results["2 relay menu"] = "already present"
        else:
            m = re.search(r"^[ \t]*cn \+= `<div class=\"seg lstI", body, re.M)
            if m:
                body = body[:m.start()] + P2 + body[m.start():]
                results["2 relay menu"] = "applied"
            else:
                results["2 relay menu"] = "FAILED (segment card template `cn += ...seg lstI` not found)"

        # Edit 3: insert the mrRow reference above the del-buttons template line
        if re.search(r"^[ \t]*mrRow \+$", body, re.M):
            results["3 template line"] = "already present"
        else:
            m = re.search(r"^([ \t]*)`<div class=\"del\">`\+", body, re.M)
            if m:
                body = body[:m.start()] + m.group(1) + "mrRow +\n" + body[m.start():]
                results["3 template line"] = "applied"
            else:
                results["3 template line"] = "FAILED (`<div class=\"del\">` template line not found)"

        text = text[:start] + body + text[end:]

    # --- Edit 4: setSegRly() after setSegBri() -------------------------------------
    if "function setSegRly" in text:
        results["4 click handler"] = "already present"
    else:
        span = func_span(text, "setSegBri")
        if span is None:
            results["4 click handler"] = "FAILED (function setSegBri() not found; appending at end works too - see patch doc)"
        else:
            close = text.find("\n}\n", span[0])
            if close < 0 or close > span[1]:
                results["4 click handler"] = "FAILED (could not find end of setSegBri())"
            else:
                ins = close + 3
                text = text[:ins] + P4 + text[ins:]
                results["4 click handler"] = "applied"

    return text, results


def find_index_js(arg):
    """Resolve the target index.js from an argument (tree root or direct path), or
    auto-discover WLED trees in the current and script directory."""
    if arg:
        p = Path(arg)
        if p.is_file():
            return p
        cand = p / "wled00" / "data" / "index.js"
        if cand.is_file():
            return cand
        sys.exit("ERROR: no index.js found at '%s' (pass the WLED tree root or the file itself)" % arg)

    roots = {Path.cwd(), Path(__file__).resolve().parent}
    found = []
    for root in roots:
        for base in [root] + [d for d in root.iterdir() if d.is_dir()]:
            cand = base / "wled00" / "data" / "index.js"
            if cand.is_file() and cand not in found:
                found.append(cand)
    if not found:
        sys.exit("ERROR: no WLED tree found here. Pass the tree root as an argument.")
    if len(found) == 1:
        return found[0]
    print("Multiple WLED trees found:")
    for n, f in enumerate(found, 1):
        print("  %d) %s" % (n, f))
    try:
        pick = int(input("Patch which one? "))
        return found[pick - 1]
    except (ValueError, IndexError, EOFError):
        sys.exit("Aborted.")


def main():
    args = [a for a in sys.argv[1:] if a != "--dry-run"]
    dry = "--dry-run" in sys.argv[1:]
    target = find_index_js(args[0] if args else None)
    print("Target: %s" % target)

    raw = target.read_text(encoding="utf-8", newline="")
    crlf = "\r\n" in raw
    text = raw.replace("\r\n", "\n") if crlf else raw

    new_text, results = apply_patch(text)

    failed = [k for k, v in results.items() if v.startswith("FAILED")]
    applied = [k for k, v in results.items() if v == "applied"]
    for k in sorted(results):
        print("  Edit %-16s %s" % (k + ":", results[k]))

    if failed:
        print("\nNOT WRITTEN: %d edit(s) could not be anchored - the WLED UI code has" % len(failed))
        print("changed. Apply those by hand using PowerManager-UI.patch.md,")
        print("then update this script's anchors/payloads to match.")
        sys.exit(1)
    if not applied:
        print("\nNothing to do - patch already fully applied.")
        return
    if dry:
        print("\nDry run: %d edit(s) would be applied, file not written." % len(applied))
        return

    backup = target.with_suffix(target.suffix + ".mrbak")
    if not backup.exists():
        shutil.copy2(target, backup)
        print("Backup written: %s" % backup)

    out = new_text.replace("\n", "\r\n") if crlf else new_text
    target.write_text(out, encoding="utf-8", newline="")
    print("\nOK: %d edit(s) applied. Rebuild the firmware now - html_ui.h regenerates" % len(applied))
    print("from index.js and the terser step validates the inserted JavaScript.")


if __name__ == "__main__":
    main()
