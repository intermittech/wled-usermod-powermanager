# Optional web UI integration

Adds a collapsible "Power relays" menu to every segment card in WLED's main UI, so relays
can be linked to segments right where the segments are controlled.

Since v1.1.0 the menu itself ships **inside the usermod**: the device serves it at
`/um.js` and the web UI injects it after every render. What must be patched into the WLED
source tree is only a small, usermod-agnostic *injection hook* (4 core files, insertions
only — designed by @blazoncek and proposed for WLED mainline; once your WLED base
includes it, skip this folder entirely).

**Apply before building your firmware:**

- Windows: run `Apply-PowerManager-UI-Patch.bat` (double-click, or pass your WLED tree path;
  `--dry-run` previews).
- Any OS: `python apply_powermanager_ui_patch.py <path-to-wled-tree>`

The patcher anchors on structural landmarks (not line numbers), is idempotent, creates
one-time `.mrbak` backups and only writes a file when every edit in it succeeds. If an
anchor fails because a WLED update restructured the code, apply the edits manually using
`PowerManager-UI.patch.md` (exact find/insert blocks and design notes).

The menu renders only when the usermod is present in the device state — firmware built
from a patched tree **without** the usermod behaves exactly like stock WLED.
