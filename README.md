# Power Manager (WLED usermod)

Advanced power switching usermod for [WLED](https://github.com/wled/WLED): named relay/MOSFET
outputs (direct GPIO or PCF8574 / AW9523(B) I2C expanders) coupled to segments, so turning a
segment off cuts that section's actual supply power - with anti-flash power sequencing, PSU
stabilization, a dedicated Master AC relay and optional main-power sync.

This usermod is heavily based on the multi_relay usermod made by blazoncek (see the Heritage section below).

Requires a recent WLED base (v0.16 / 17.0-dev nightlies) with the external-usermod build system.

Designed with the [QuinLED Dig-Next-2](https://dig-next.info/dig-next-2.html) WLED controller in mind, enabling the per port individual power output control hardware feature.

## Installation

**As an external usermod (recommended):** add the repository to `custom_usermods` in your
`platformio_override.ini` environment - WLED's build system fetches and links it automatically:

```ini
[env:my_build]
extends = env:esp32dev
custom_usermods = https://github.com/intermittech/wled-usermod-powermanager.git
```

(This always builds the newest version automatically. See `examples/platformio_override.ini`
for complete environments including all compile-time options.)

**Or as a local copy:** copy this repository into your WLED tree as
`usermods/PowerManager/` and use `custom_usermods = PowerManager`.

### Optional web UI integration (segment cards)

<img src="images/segments-screenshot.png" align="right" width="230" alt="The Power relays menu on a segment card">

The "Power relays" menu on the segment cards (shown on the right): link power relays right
where you control the segment, with the current assignment of every relay at a glance -
green for this segment, dimmed for relays serving another one.

This part requires a small patch to one WLED core file (`wled00/data/index.js`), applied
**before building the firmware**. It cannot live inside the usermod because that file is
compiled into WLED's web UI at build time. It is entirely optional: without it everything
works and links are configured on the Usermods settings page instead.

See [`ui-patch/`](ui-patch/) for the automatic patcher (anchor-based, idempotent, with
backup) and the manual patch documentation. Firmware built from a patched tree **without**
this usermod behaves exactly like stock WLED.

<br clear="right"/>

## Heritage

This usermod grew out of WLED's built-in `multi_relay` usermod, written and maintained by
**@blazoncek** (with contributions credited in the [change log](https://github.com/intermittech/wled-usermod-powermanager/wiki/Change-Log),
which preserves that lineage). Power Manager is maintained separately and is **not** meant to
be installed alongside `multi_relay` - it uses its own configuration (`PowerManager`) and JSON
API key, and **automatically migrates settings** saved by a previous `multi_relay` installation
on first boot. The MQTT topics (`.../relay/N`) and the HTTP `/relays` API are kept unchanged so
existing automations and Home Assistant entities keep working.

## I2C port expanders

Select the expander type in the usermod settings (or at compile time). Relays are attached to expander ports using virtual pin numbers starting at 100:

| Expander | Ports | Virtual pins | I2C addresses |
|----------|-------|--------------|---------------|
| PCF8574  | 8 (P0-P7) | 100-107 | 0x20-0x27 (PCF8574A: 0x38-0x3F) |
| AW9523   | 16 (P0_0-P0_7, P1_0-P1_7) | 100-107 (P0_x), 108-115 (P1_x) | 0x58-0x5B (AD1/AD0 straps) |

Expander use requires global I2C pins to be defined in LED & Hardware settings. After changing
the expander type, save and re-open the Usermods settings page so the pin dropdowns show the
matching port names.

### AW9523 notes

* The chip is verified via its ID register at boot; the Info page shows whether it was found.
* Only the ports assigned to relays are (re)configured (GPIO mode, output direction, interrupts masked) - other ports of the chip are left untouched, so they remain available for other purposes.
* The P0_x port is open-drain by default in hardware. The usermod configures it as push-pull by default (recommended when driving relay/optocoupler inputs directly). Untick `AW9523-pushpull` (or set `-D AW9523_P0_PUSHPULL=false`) if your board relies on open-drain outputs with external pull-ups. P1_x is always push-pull.
* The RSTN pin of the AW9523 has an internal 100k pull-*down* - it must be tied high (to VCC) on the board or the chip stays in reset.

## Features & behavior

* **Segment coupling** - link any relay to a segment; the section's supply power follows the segment's on/off state
* **Master AC relay** - dedicated relay 0 slot for the PSU trigger: on while any segment is on, always the last to cut, 5s anti-cycling off-delay by default
* **Main power sync** (optional) - WLED's power button mirrors reality: off when the master cuts, restored by switching a segment on
* **Power sequencing** - per-relay on/off delays, fade-aware power-off, anti-flash black frames around power-on, PSU stabilization window, minimum off-time against rapid-cycle flashes
* **Named relays** - up to 16 outputs named after your physical ports, shown in the segment menu, Info page and Home Assistant
* **Take over all relays** (optional) - unconfigured relays stay off until given a role
* **Preset-carried links** (advanced) - presets can re-map relay links for layout-switching setups

Full details, timing behavior and caveats: **[Segment Coupling and Power Sequencing](https://github.com/intermittech/wled-usermod-powermanager/wiki/Segment-Coupling-and-Power-Sequencing)** (wiki)

## Configuration

Everything is configurable on the Usermods settings page (general, expander, broadcast and
power-sequencing sections, with the Master AC relay as the first relay block), and every
default can be baked into prebuilt firmware via `POWERMANAGER_*` compile-time defines.

All options and defines: **[Configuration Reference](https://github.com/intermittech/wled-usermod-powermanager/wiki/Configuration-Reference)** (wiki)

## APIs & Home Assistant

Relays are controllable via the HTTP `/relays` endpoint, the JSON API (`{"PowerManager":...}`)
and MQTT (`.../relay/N`); externally controlled relays appear in Home Assistant automatically
via MQTT autodiscovery.

Endpoints, payloads and topics: **[API Reference](https://github.com/intermittech/wled-usermod-powermanager/wiki/API-Reference)** (wiki)

## Change log

See the **[Change Log](https://github.com/intermittech/wled-usermod-powermanager/wiki/Change-Log)** in the wiki - the full lineage from the 2021 multi_relay beginnings to the current release.

## License

Licensed under the EUPL-1.2, like WLED itself - see [LICENSE](LICENSE).
