# Deskpad — Implementation Plan

Living document. Phases below were derived from the [grilling session](docs/adr/)
that produced [CONTEXT.md](CONTEXT.md), the four ADRs in [docs/adr/](docs/adr/),
and [docs/config-sample.yaml](docs/config-sample.yaml) (target schema preview).

Status legend: ✅ done · 🟡 in progress · ⏳ planned · 💤 deferred to a later phase

---

## Phase 1 — NVS config ✅
Replace the compile-time `LCD_BINDINGS` / `ENCODER_BINDINGS` tables with a
JSON blob in NVS, loaded at boot. Schema-versioned. Defaults stamped on
first boot.

- `config.h/c` — schema, NVS persistence, parse/serialise
- `actions.c` — reads bindings from `config_get()` instead of C arrays

## Phase 2 — Ethernet + SPA + OTA ✅
Wired networking; mDNS as `deskpad.local`; embedded single-page app for
config editing; chunked OTA upload.

- `network.h/c` — IP101GRI PHY on RMII; **PHY drives the clock to avoid
  MPLL conflict with PSRAM at 200 MHz** ([espressif/esp-idf#18377](https://github.com/espressif/esp-idf/issues/18377))
- `http_server.h/c` — `GET/PUT /api/config`, `GET /api/enums`, `POST /api/ota`, `POST /api/reboot`
- `spa_index.html` — vanilla-JS SPA; embedded via `EMBED_FILES`
- `partitions.csv` — two 7 MB OTA slots + NVS + SPIFFS + rollback flow

## Phase 3 — Schema v2 + binding dispatcher + scope ✅
Bindings carry `scope` (global / host:PC1 / host:PC2), shared `entity`,
typed `display` + `binding`. KVM topology moves from `ddc.h` `#define`s
into NVS config. Auto-migrates v1 configs forward.

- `config.h/c` — tagged-union binding/renderer types, v1→v2 migration
- `actions.c` — binding dispatcher; scope filter against active Host
- `spa_index.html` — full v2 editor (scope/entity/renderer/binding sub-forms)
- ADRs:
  [0001](docs/adr/0001-u38-is-the-kvm.md),
  [0002](docs/adr/0002-host-is-kvm-slot-not-machine.md),
  [0003](docs/adr/0003-hybrid-binding-scope.md),
  [0004](docs/adr/0004-ethernet-no-wifi.md)

## Phase 4a — host_state + monitor renderer ✅
First state-reactive renderer. KVM-toggle / kvm-select keys now light up
to reflect the active Host instead of showing a static JPEG.

- `host_state.h/c` — Active Host get/set/subscribe (subscriber list)
- `monitor_render.h/c` — config-driven; one live_key per `REND_MONITOR`
  slot; subscribes to `host_state` and invalidates on change

## Phase 4b — clock renderer in registry ✅
Move clock from hardcoded slot (page 2 / slot 5) into the same pattern
as monitor_render. Any slot configured `renderer: clock` becomes a clock.

- `clock_render.h/c` — replaces the old `clock_key.c`. 1 Hz timer.
- Default config restores a clock at page 2 / slot 5 so fresh boots
  match the old behaviour.

## Phase 5 — HID via UART link ✅ (firmware) · ⏳ (hardware bring-up)
Make the page 1 media keys and page 2 system chords actually fire on
the active host. UART link to the RP2350; reuses the existing wire
protocol from kvm-switcher.

- `hid_link.h/c` — UART1 on default pins (TX=GPIO 8, RX=GPIO 9), 1 Mbps
- `hid_keys.h/c` — `parse_chord("ctrl+shift+m")` + consumer/system name lookups
- `actions.c` — `BIND_HID_CHORD` and `BIND_HID_CONSUMER` send via link

**Pending physical work:** wire P4 GPIO 8 ↔ RP2350 GP1, P4 GPIO 9 ↔ RP2350 GP0,
common GND. Plug RP2350 into host PC. Test on hardware.

---

## Phase 6 — External KVM detection ✅
Notice when *something else* switched the U38 (physical input button on
the monitor, host wake-from-sleep auto-switch) and follow on the
secondary monitor. Combines the RP2350's re-enumeration signal (fast)
with a slow DDC poll on the U38 (belt-and-braces).

- Forward `MSG_USB_STATUS` events from `hid_link.c` into `host_state`
- Add 5 s DDC poll task; verify against `monitor_b` (the U24 — see
  [ddc-wiring memory](C:/Users/chris/.claude/projects/c--dev-deskpad/memory/project_ddc_wiring.md))

## Phase 7 — MQTT consume + `ha` binding ✅
- `secrets.h/c` — separate NVS key for MQTT creds.
- `mqtt.h/c` — esp-mqtt wrapper + subscription registry (topic →
  callback). Reconnects automatically, re-subscribes all on reconnect.
- `bulb_render.h/c` — first state-reactive renderer; subscribes to
  HA-derived state topics or explicit `state_topic` overrides.
- `ha` binding dispatch: publishes intents to `deskpad/cmd` topic;
  user adds one HA automation to dispatch to the service.
- `/api/credentials` GET (masked) + PUT (write-only password).
- `thermostat` / `text_value` renderers stubbed in the renderer enum;
  implementation deferred until concrete topics are wired.

## Phase 8 — HA discovery ✅
- `ha_discovery.h/c` — publishes MQTT discovery for sensors
  (`active_host`, `current_page`, `hid_link`, `usb_mounted`,
  `ddc_a`, `ddc_b`); periodically re-publishes state every 5 s and
  immediately on host_state change.
- Press / encoder events publish to `deskpad/event/...` topics
  (HA users wire MQTT triggers from there).

## Phase 9 — Polish 🟡 (partial)
- README rewrite ✅
- `docs/api.md` — full HTTP API reference ✅
- Auth on Web UI: deferred — defaulted to no-auth + LAN-only.
- Live rebind on PUT /api/config: deferred. Most renderers still
  require a reboot to pick up renderer-or-binding-type changes. Binding
  type changes for already-bound slots + scope changes do apply
  immediately. Renderer pipeline reload is the missing piece.
- Press animation for live renderers: deferred. key_anim's darken-on-
  press path only applies to static slots; live renderers don't get
  the press visual feedback yet.
- `static_jpeg` reads `image` field properly: deferred — currently
  the slot still resolves the embedded JPEG by position (page/slot).
- Renderer cleanup (drop stale fields when renderer changes in SPA):
  deferred — minor.
- Image upload for `static_jpeg`: deferred — requires storage flow.

## Phase W — Web UI v2 + public HTTP API ✅ (W.1 + W.3 done)

Current SPA is a single-screen config dump. Goals:
1. Surface deskpad state on a landing page (active Host, current page,
   subsystem health, recent events) — not the config editor.
2. Split config editing into per-page screens (one screen per AKP page +
   one for encoders + one for KVM topology + one for credentials).
3. Cleanly separate the **API** from the **SPA**: every action the SPA
   takes is a stable HTTP call you can hit directly with `curl` /
   scripts / Home Assistant.
4. Pick up the styling tokens (colour palette, typography, layout
   density) from the old kvm-switcher `web_ui.h` so it feels coherent
   with what was there before.

### W.1 — Public HTTP API (do first; SPA depends on it)
Define the contract before redesigning the front-end. Endpoints to add:

| Method | Path | Purpose |
|---|---|---|
| GET  | `/api/state`                                   | Runtime: active_host, current_page, uptime, ip, link/USB/DDC status |
| GET  | `/api/health`                                  | { ddc_a, ddc_b, hid_link, network, mqtt } |
| GET  | `/api/log/recent?n=100`                        | Tail of in-memory log ring (debug) |
| POST | `/api/action/kvm/toggle`                       | Flip PC1↔PC2 |
| POST | `/api/action/kvm/select` { target }            | Switch directly to PC1 or PC2 |
| POST | `/api/action/page` { page }                    | Switch AKP page |
| POST | `/api/action/ddc` { bus, vcp, value }          | Raw DDC write |
| POST | `/api/action/hid/chord` { chord }              | Fire a chord |
| POST | `/api/action/hid/consumer` { name }            | Fire a consumer-control code |
| PUT  | `/api/config/page/{n}/slot/{s}` { slot_obj }   | Update one slot without round-tripping the whole config |
| PUT  | `/api/config/encoder/{n}/page/{p}` { binding } | Update one encoder binding |
| GET  | `/api/credentials`                             | Masked: shows shape but no secret values |
| PUT  | `/api/credentials` { mqtt: {...} }             | Write secrets (password omitted = keep existing) |

Existing endpoints (`/api/config` GET/PUT, `/api/enums`, `/api/ota`,
`/api/reboot`) stay. Document everything in `docs/api.md`.

### W.2 — Styling tokens (extracted from old kvm-switcher web_ui.h)

Dark-dense aesthetic. System font, monospace for technical values. Pair
of brand accents per "input" (green = PC1, purple = PC2). All colour
chosen, not auto-themed.

**Palette:**
```
Background           #111
Card                 #1a1a1f
Card border          #2a2a30
Recessed bg          #111115     (switcher rail, monitor row, inputs)

Text primary         #ccc
Text secondary       #888
Text tertiary        #666
Text dim             #555 / #444

PC1 accent (green)   #1D9E75 (filled) / #5DCAA5 (lit text) / #0F2D24 (lit bg) / #1D9E7560 (border)
PC2 accent (purple)  #7F77DD (filled) / #AFA9EC (lit text) / #1E1B3A (lit bg) / #7F77DD60 (border)

Status — awake       #5DCAA5 text / #0F2D24 bg / #1D9E75 dot
Status — asleep      #EF9F27 text / #2D2106 bg
Status — fail        #E24B4A dot  / #cc7a82 text / #2a1418 bg

Button primary       bg #252530 / border #3a3a44 / text #ccc
Button hover         #30303c
Button secondary     bg #1a1a24 / border #2a2a34 / text #888
Button danger        bg #2a1418 / border #5a2a30 / text #cc7a82
```

**Typography:**
- Font: `-apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif`
- Top title: 14px / weight 500 / `#888` / .06em letter-spacing / uppercase
- Main numbers (PC1 / PC2 labels): 20px / weight 500
- Body labels: 11-12px
- Monospace for tech values (IP, hotkey, ports, VCPs, MQTT host)

**Layout:**
- Single centered card, max-width 440px
- Border-radius scale: 5px (tags), 6-7px (fields/inputs), 8px (rows), 10-12px (cards)
- 1.5rem padding, 1px borders
- Mobile breakpoint at coarse-pointer or `max-width: 768px` scales up tap targets

**Patterns to keep:**
- Top: name + status dot (top-right)
- Recessed sub-areas (deeper `#111115` against `#1a1a1f` card)
- Status tags (rounded 5px pills with semantic colour)
- `<details>` collapse for secondary config (clean for per-section editing)
- Foot row: uptime + small inline links (e.g. firmware update)

**Patterns to drop:**
- Captive-portal flow ("Forget WiFi & reboot") — Ethernet now, no AP fallback
- Hotkey-capture UI — deskpad's input source is the AKP, not a host-side hotkey
- The "single-card, all-on-one-screen" constraint — deskpad has more state
  to surface; multi-route SPA per W.3 below

### W.3 — SPA rebuild
- Light client-side router (`pushState` + hash, no framework yet)
- Routes:
  - `/`           — landing dashboard (status + quick actions + recent events)
  - `/page/0..2`  — per-page slot editor (6 cards, one page at a time)
  - `/encoders`   — encoder bindings per-page-per-encoder
  - `/kvm`        — KVM topology / input mapping
  - `/credentials`— secrets (separate so it can be gated later)
  - `/firmware`   — OTA upload + reboot
- Shared header showing live state (active Host, current page, link
  status) — driven by polled or SSE `/api/state` data.
- Each route is one focused screen rather than a giant form.

## Phase N — Notifications ✅

External events (doorbell, motion, calendar reminders, weather alerts,
arbitrary HA automation) surface on the AKP as visual notifications.
Built on top of Phase 7's MQTT infrastructure — no new transport needed,
just a subscription + rendering layer.

### Display levels

Three styles per notification, picked by config:

- **Takeover** — replaces all 6 LCD keys with a single notification
  screen (large icon + label). Highest visibility. Right for doorbell,
  alarm, calendar "meeting starts in 1 minute".
- **Overlay** — replaces one specific slot with a notification icon
  for the duration. Medium visibility. Right for motion detection,
  email arrived, calendar "meeting in 5 min".
- **Pip** — small coloured dot in the corner of a designated slot,
  doesn't replace the slot's content. Low visibility. Right for
  ambient indicators (e.g. "porch light is on").

### Triggers

Per notification, an MQTT topic + payload matcher:
```yaml
trigger:
  topic: homeassistant/binary_sensor/doorbell/state
  on_value: 'on'              # exact match, or jsonpath later
```

### Dismissal

One or more of:
- `auto`            — clears after `duration_s` (e.g. 30 s)
- `any_key`         — any AKP press dismisses (suppresses the underlying
                       binding for that press)
- `source_off`      — clears when the MQTT topic publishes the "off"
                       value (e.g. motion stops)

### Stacking / priority

- Queue: at most one active notification at a time.
- `priority: high|medium|low`. Higher priority interrupts lower (lower
  re-queues for after dismissal).
- Same-priority: FIFO; only one shown at a time.

### Config shape (target YAML)

```yaml
notifications:
  - id: doorbell
    priority: high
    trigger: { topic: homeassistant/binary_sensor/doorbell/state, on_value: 'on' }
    display:
      style: takeover
      icon: doorbell.jpg
      label: DOORBELL
      colour: '#E24B4A'
      duration_s: 30
      dismissable: [any_key, source_off]

  - id: front_motion
    priority: medium
    trigger: { topic: homeassistant/binary_sensor/front_motion/state, on_value: 'on' }
    display:
      style: overlay
      target_slot: 0          # which AKP key to overlay
      icon: motion.jpg
      duration_s: 10
      dismissable: [auto]

  - id: porch_light_on
    priority: low
    trigger: { topic: homeassistant/light/porch/state, on_value: 'on' }
    display:
      style: pip
      target_slot: 5
      colour: '#5DCAA5'
      dismissable: [source_off]
```

### Architecture

- `notify.h/c` — subscriber, queue, dismissal timer, render overrides.
- Shares a common "overlay mechanism" with the **boot-time health
  surface** (above) — both want to inject visuals over key_anim's
  normal rendering. Worth factoring as a shared `overlay` primitive
  that takes priority over slot renderers.
- The `any_key` dismissal hook lives in `actions.c`: if a notification
  is active and `any_key` is in its dismissable list, the next
  `EV_SLOT_PRESS` cancels the notification and is NOT dispatched to
  the slot's binding.
- A small `/api/notifications` endpoint exposes active + recent
  history for the SPA dashboard.

### Not in v1

- Sound — deskpad has no audio output. Could route to HA / a separate
  device for chime-on-event but that's an HA concern, not deskpad.
- Reply / quick-action buttons (e.g. "doorbell rings → tap to start
  CCTV PIP overlay"). Possible in a later phase; for now the user
  dismisses and presses whatever binding they normally would.
- Per-page notification routing (e.g. "show motion overlay only on
  page 0"). Single-overlay-mechanism for v1; route everywhere.

---

## Cross-cutting items not yet phased ⏳

### Boot-time / runtime health surface
DDC bus failures, HID link down, MQTT disconnected, NVS read errors —
all currently go to the console only. Need to surface them on the AKP
itself so a desk-mounted device with no serial monitor still reports
problems.

Proposed shape (defer the implementation):
- `system_status.h/c` — small registry of subsystem health
  (`{name, severity, message}`). Subsystems publish updates on
  state change.
- AKP rendering: on any **error**-severity entry, overlay a red banner
  on AKP key 0 of the current page with a brief message, cycling
  through entries if there are multiple. Auto-clears when the fault
  clears.
- SPA: a status panel at the top of the editor listing all current
  health entries (good and bad).
- Bootstrap: hold all-errors visible for a few seconds at boot before
  fading into normal rendering — gives the user a chance to spot DDC /
  link / network issues immediately after flash.
- Subsystems to instrument first: DDC bus A/B presence, HID link
  heartbeat, network (link + IP), MQTT (when Phase 7 lands).

### PIP / PBP control of the U38
The U38 supports PIP/PBP via vendor-area DDC/CI VCP codes (typically
in the 0xE0-0xFF range, often 0xE9 for the PIP mode selector). Exact
codes are not published by Dell for this exact model; discover
empirically with `ddcutil` (Linux) or a deskpad discovery sketch. Once
known, slot the values into the firmware's named-command table:
- `pip_off`, `pip_small`, `pip_large`, `pbp` → `{vcp: 0xE9, value: …}`
- The generic `{type: ddc, bus: A, vcp: 0x??, value: 0x??}` binding
  already supports raw VCP writes for prototyping, so PIP control can
  be wired up before the codes are catalogued.
- Worth confirming first: does the U38 even allow the input
  combinations you'd want for PIP (e.g. PIP-an-HDMI-source on top of
  the currently-active USB-C source)? Dell firmware is picky about
  source combinations — check the OSD menu.

---

## Open architectural questions

- **HID hold-style** (true PTT). Reserved as `hold: true` on hid bindings;
  needs RP2350-side key-up emission. Deferred — tap-style mute is
  sufficient for the Teams use case.
- **PIP / CCTV from deskpad.** Wants an LT8912B MIPI-DSI→HDMI bridge
  (e.g. [Olimex board](https://www.olimex.com/Products/IoT/ESP32-P4/MIPI-HDMI/open-source-hardware)).
  Check LT8912B DDC tunnelling before ordering if cable-count matters.
- **DDC awake-detection** on bus A (U38) is untested — heritage code only
  used bus B (U24). Verify when Phase 6 lands.

---

## Backlog / future ideas

Captured but not committed to a phase yet. Add to the appropriate phase
when picked up.

### Hold-modifier gestures
Bind an action to "hold one of the three side keys + rotate / press one
of the encoders" so the existing 9 encoder events × 3 pages can be
multiplied without needing new physical buttons. First concrete use:

- **Brightness via gesture**: hold side-key for page 0 + rotate the
  main (big) encoder → change AKP backlight brightness (calls
  `actions_brightness_set`). Hold + press → toggle brightness between
  a "bright" and "dim" preset.

Architecture sketch:
- Track currently-held side key in `host_state` or a small
  `input_state` module.
- In actions.c encoder dispatch, if a side key is held, route the
  event through a "modifier" binding map rather than the page's
  normal encoder bindings.
- Config schema adds an optional `modifier_bindings` section per
  side-key-held state. Per-page schema becomes per-modifier-state +
  per-page.

### Sit / stand desk integration
The desk is likely controllable (model TBD). Useful integrations:
- An AKP key bound to "stand up" / "sit down" presets, talking via
  HA (most modern smart desks have HA support via dedicated
  integrations or Bluetooth/Linak protocols).
- A `text_value` renderer on a key showing current desk height,
  pulled from an HA sensor topic.
- A scheduled "stand up" notification (using the Phase N notification
  takeover) once per hour during work blocks.

Defer until: desk make/model confirmed and HA integration verified.

### Encoder-bound brightness (simpler alternative to gesture)
If hold-modifier gestures feel like overkill, simpler: dedicate one of
the small encoders (encoder 1 or 2) globally to AKP brightness. Add a
new `brightness` binding type alongside `ddc`/`hid`/`ha`.

### AKP sleep on monitor-off (with wake-on-touch)
When the monitors go to sleep, the AKP screens should follow — but
buttons must still wake the device on touch. Don't fully cut power.

Approach (most pieces exist already):
- Poll the U24's `VCP 0xD6` (power mode) periodically — same hook
  Phase 6 introduced for KVM detection.
- On transition to "monitor asleep", call `akp03e_sleep()` (HAN
  opcode — already in the component).
- On the next AKP button event (which should still fire on USB intr-IN
  even while the screens are blanked), call a new `akp03e_wake()`
  function that undoes the HAN.

Open question: the wake opcode isn't documented. The vendor software
has explicit sleep/wake controls; capture a pcap of that toggle to see
which opcode the device responds to. Could be:
- a no-op CRT packet (`DIS` re-init?),
- a brightness `LIG`,
- a dedicated wake command we haven't seen yet.

Also worth deciding: does deskpad sleep the AKP when *any* monitor
sleeps, or only when *both* do? Probably both — "user has left the
desk" is the trigger semantic.

---

## Out of repo

- Old kvm-switcher GitHub repo is **private** (effectively archived).
  Credentials it contained were rotated before privacy switch.
- Local mirror `C:/dev/_kvm-cleanup` from the failed `git filter-repo`
  attempt has been removed.
