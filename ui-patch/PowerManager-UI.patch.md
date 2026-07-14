# PowerManager segment-coupling — core UI patch for `wled00/data/index.js`

The segment-coupling feature (power_manager usermod, milestone 2) is almost entirely usermod-only.
The **one** core file it touches is `wled00/data/index.js` (the main web UI), which adds a
"Power relays" checkbox row to each segment card. This file is overwritten whenever a new WLED
nightly source tree is unpacked — **re-apply the four edits below after every nightly refresh.**

The row only renders when the Power Manager usermod is present in `/json/state` (`state.PowerManager`),
so builds without the usermod are visually and behaviorally unchanged.

`html_ui.h` regenerates automatically on the next PlatformIO build (`pre:pio-scripts/build_ui.py`
runs `npm run build`). Success indicator in build log: `Writing wled00/html_ui.h`.
Terser minification doubles as a JS syntax check — a broken edit fails the build step.

---

## Edit 1 — relay list, top of `populateSegments()` (~line 748)

Find:
```js
function populateSegments(s)
{
	var cn = "";
	let li = lastinfo;
	segCount = 0; lowestUnused = 0; lSeg = 0;
```

Append below:
```js

	// Power Manager usermod: relays that can be coupled to a segment (row is hidden when the usermod is absent)
	let mrRelays = [];
	if (s.PowerManager) mrRelays = (Array.isArray(s.PowerManager.relays) ? s.PowerManager.relays : [s.PowerManager])
		.filter(r => r.seg !== undefined && r.seg != 99); // 99 = "any segment" mode, managed in usermod settings
```

## Edit 2 — build the collapsible relay menu per segment card (directly after the `let sndSim = ...;` block, ~line 830)

Append after the `sndSim` statement (inside the `for (var inst of (s.seg||[]))` loop):
```js
		let mrRow = "";
		if (mrRelays.length) {
			// keep the relay list open across re-renders (each link click re-renders the cards from state)
			let rlyOpen = gId(`seg${i}rlyl`) ? !gId(`seg${i}rlyl`).classList.contains('hide') : false;
			let lnk = mrRelays.filter(r=>r.seg==i).map(r=>r.name?r.name:"Relay "+r.relay).join(", ");
			mrRow = `<div class="check revchkl" style="cursor:pointer;" title="Link power relays: their output is cut when this segment is off" `+
						`onclick="gId('seg${i}rlyl').classList.toggle('hide');gId('seg${i}rlyc').classList.toggle('exp');">`+
					`Power relays: ${lnk?lnk:"none"}`+
					`<i class="icons e-icon${rlyOpen?" exp":""}" id="seg${i}rlyc" style="position:absolute;left:0;top:3px;transition:transform .3s;">&#xe395;</i></div>`+
					`<div id="seg${i}rlyl" class="${rlyOpen?'':'hide'}" style="margin-left:16px;">`;
			for (const r of mrRelays) {
				// hint where the relay is currently linked: this segment (accent) or another one (dimmed)
				let other = "";
				if (r.seg == i) {
					other = ` <span style="color:var(--c-g);font-size:smaller;">(this segment)</span>`;
				} else if (r.seg >= 0) {
					let os = (s.seg||[]).find(q => q.id == r.seg);
					other = ` <span style="color:var(--c-d);font-size:smaller;">(${os&&os.n ? os.n : "Segment "+r.seg})</span>`;
				}
				mrRow += `<label class="check revchkl">${r.name?r.name:"Relay "+r.relay}${other}`+
							`<input type="checkbox" id="seg${i}rly${r.relay}" onchange="setSegRly(${i},${r.relay})" ${r.seg==i?"checked":""}>`+
							`<span class="checkmark"></span></label>`;
			}
			mrRow += `</div>`;
		}
```

(Styling note: the summary line reuses the `check revchkl` classes — not `lbl-l` — so it matches
the Reverse/Mirror rows in font size and left alignment. A WIcons expand chevron (`&#xe395;`)
sits absolutely at left:0 where a checkbox would be, and toggles the global `.exp` class to
rotate 180° on open, mirroring the segment-card expander. The checklist is indented 16px.)

## Edit 3 — insert the row into the card template (just above the delete/repeat buttons, ~line 900)

Find (end of the "Transpose/Mirror effect" label, before the del div):
```js
					`<span class="checkmark"></span>`+
					`</label>`+
					`<div class="del">`+
```

Change to:
```js
					`<span class="checkmark"></span>`+
					`</label>`+
					mrRow +
					`<div class="del">`+
```

(Note: `<span class="checkmark">` appears several times in the file — this is the one directly
followed by `` `<div class="del">` ``.)

## Edit 4 — click handler (after `function setSegBri(s) {...}`, ~line 2430)

Append after `setSegBri`:
```js

// Power Manager usermod: (un)couple relay r to/from segment s (power cutoff follows segment on/off)
function setSegRly(s, r)
{
	var lnk = gId(`seg${s}rly${r}`).checked;
	var obj = {"PowerManager": {"relay": r, "seg": lnk ? s : -1}};
	requestJson(obj);
}
```

---

## How it works / design notes

* Data source: the usermod's `addToJsonState()` puts `{relays:[{relay,state,seg,name?},...]}` into
  every state response (`name` only present when set); `requestJson()` always posts `v:true`, so the
  UI re-renders segment cards from the device's authoritative state after every click (takeovers
  re-sync automatically).
* The menu is collapsed by default and shows the linked relay names in its summary line; it stays
  open across the post-click re-render (open/closed state is read back from the old DOM, same trick
  the cards use for their own expanded state). Unnamed relays display as "Relay N".
* Checking a box sends `{"PowerManager":{"relay":R,"seg":<segId>}}`; unchecking sends `"seg":-1`.
  The usermod persists changes via `configNeedsWrite`.
* Relays in "any segment" master mode (`seg == 99`) are deliberately hidden from the cards so the
  master-PSU relay can't be accidentally re-linked; manage it from Usermod settings.
* Known cosmetic edge: a `POWER_MANAGER_MAX_RELAYS=1` build reports relay 0 in the state object even
  when no pin is assigned (pre-existing usermod API shape), which would show one checkbox.
