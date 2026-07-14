# Optional web UI integration

Adds a collapsible "Power relays" menu to every segment card in WLED's main UI, so relays
can be linked to segments right where the segments are controlled. This cannot ship inside
the usermod - it modifies one WLED core file (`wled00/data/index.js`), which is compiled
into the firmware's web UI at build time.

**Apply before building your firmware:**

- Windows: run `Apply-PowerManager-UI-Patch.bat` (double-click, or pass your WLED tree path;
  `--dry-run` previews).
- Any OS: `python apply_powermanager_ui_patch.py <path-to-wled-tree>`

The patcher anchors on structural landmarks (not line numbers), is idempotent, creates an
`index.js.mrbak` backup and only writes when every edit succeeds. If an anchor fails because
a WLED update restructured the UI, apply the edits manually using
`PowerManager-UI.patch.md` (exact find/insert blocks and design notes).

The menu renders only when the usermod is present in the device state - firmware built from
a patched tree **without** the usermod behaves exactly like stock WLED.
