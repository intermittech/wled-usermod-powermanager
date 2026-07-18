# WLED usermod UI injection hook — manual patch reference

Since PowerManager v1.1.0, the "Power relays" segment-card menu is generated **by the
usermod itself**: the device serves it as JavaScript at `/um.js` and the main web UI
injects it after every state render. The WLED source tree only needs this small,
usermod-agnostic hook (designed by @blazoncek and proposed for WLED mainline in
[wled/WLED#5741](https://github.com/wled/WLED/pull/5741) — once your WLED base includes
it, no patching is needed at all).

`apply_powermanager_ui_patch.py` applies everything below automatically. Use this
document when the script reports a failed anchor (i.e. a WLED update moved the
landmarks), or if you want to review exactly what changes.

Four files are touched. Insertions only — no existing line is modified or removed.

---

## 1. `wled00/data/index.js` (3 edits)

### 1a. The loader and the exception shield — insert directly **after** the `getURL()` function:

```js
function getURL(path) {
	return (loc ? locproto + "//" + locip : "") + path;
}
```

insert:

```js

// load usermod UI inject code (served by the device when usermods provide any);
// umInjectSafe(state) is then called after every state render, see readState()
function loadUmInject(s) {
	if (gId("um")) return; // already loaded
	let scE = d.createElement("script");
	scE.id = "um";
	scE.src = getURL("/um.js");
	scE.async = false;
	scE.onload = () => umInjectSafe(s); // render once with the state captured at load time
	scE.onerror = (ev) => {
		console.log("Usermod inject script not present or failed to load", ev);
	};
	d.body.appendChild(scE);
}

// run usermod UI inject code, shielding the UI from exceptions in usermod-provided JS
// (an uncaught throw here would abort readState() and trigger requestJson()'s retry loop)
function umInjectSafe(s) {
	if (typeof umInject != "function") return;
	try {
		umInject(s);
	} catch (e) {
		console.error("Usermod UI inject error:", e);
	}
}
```

### 1b. The render hook — at the **end of `readState(s)`**, between `updateUI();` and `return true;`:

```js
	updateUI();
	umInjectSafe(s); // usermod UI injections (see loadUmInject())
	return true;
}
```

### 1c. The load trigger — in `requestJson()`'s fetch handler, directly **after** `readState(s);`:

```js
			var s = json.state ? json.state : json;
			readState(s);
			if (json.info && json.info.u) loadUmInject(s); // usermods present: load their UI inject code

			reqsLegal = true;
```

(`info.u` is present when the build contains usermods, so stock builds never fetch the script.)

---

## 2. `wled00/fcn_declare.h` (3 edits)

### 2a. Directly **before** `class Usermod {` (after the `um_data_size` constant):

```cpp
// usermods can inject JS into the main web UI via addUIInjectCode() (served at /um.js);
// external usermods can test this macro to stay compatible with older WLED bases
#define WLED_ENABLE_UM_UI_INJECT
```

PowerManager guards its menu code with `#ifdef WLED_ENABLE_UM_UI_INJECT`, so the usermod
also compiles cleanly against stock WLED bases without this hook (the menu is simply absent
and relays are linked on the settings page instead).

### 2b. In `class Usermod`, after the `onStateChange` virtual:

```cpp
    virtual void onStateChange(uint8_t mode) {}                              // fired upon WLED state change
    virtual void addUIInjectCode(Print &dest) {}                             // print JS code injecting UI elements into the main web UI (served at /um.js, run after every state render)
```

### 2c. In `namespace UsermodManager`, after the `onStateChange` declaration:

```cpp
  void onStateChange(uint8_t);
  void addUIInjectCode(Print &dest);
```

---

## 3. `wled00/um_manager.cpp` (1 edit)

After the `UsermodManager::onStateChange` implementation line:

```cpp
void UsermodManager::addUIInjectCode(Print &dest) { for (auto mod = DYNARRAY_BEGIN(usermods); mod < DYNARRAY_END(usermods); ++mod) (*mod)->addUIInjectCode(dest); } // collect usermod UI inject JS (served at /um.js)
```

---

## 4. `wled00/wled_server.cpp` (1 edit)

In `initServer()`, after the `/freeheap` handler block:

```cpp
  // usermod UI inject code: the main UI loads this script and calls umInject(state) after
  // every state render, letting usermods add their own elements without patching index.js
  server.on(F("/um.js"), HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncResponseStream *response = request->beginResponseStream(FPSTR(CONTENT_TYPE_JAVASCRIPT));
    response->addHeader(FPSTR(s_cache_control), F("no-store"));
    response->addHeader(F("Expires"), F("0"));
    response->print(F("function umInject(s){"));
    UsermodManager::addUIInjectCode(*response);
    response->print(F("}"));
    request->send(response);
  });
```

---

## Design notes

- **How it fits together:** every usermod may override `addUIInjectCode(Print&)` and print
  plain JavaScript. `/um.js` concatenates all of it inside `function umInject(s){...}`.
  The main UI loads the script once and calls `umInject(s)` with the fresh state object at
  the end of every `readState()` — so injected elements survive full UI re-renders
  (`populateSegments()` rebuilds the segment cards on every state update), for both fetch
  and WebSocket updates.
- **Exception shielding:** all calls go through `umInjectSafe()`, which wraps `umInject()`
  in try/catch — a throwing usermod script would otherwise abort `readState()` and set off
  `requestJson()`'s 10-retry loop, flooding the device with requests.
- **Zero cost when unused:** usermods that don't override the virtual contribute nothing;
  without any usermod the endpoint serves an empty function, and `info.u` gating means
  stock builds never even request it.
- **For usermod authors:** injected code runs on the *main UI* page — only its globals
  exist (`d`, `gId`, `requestJson`, ...). Settings-page helpers like `cE()` are **not**
  available; create elements with `d.createElement()`.
- **After patching, rebuild the firmware**: `html_ui.h` is regenerated from `index.js`,
  and the terser minification step doubles as a syntax check of the inserted JS.
