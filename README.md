# deskpad

A desk control surface that coordinates a workstation centred on a single
KVM-capable monitor. Six LCD-backed buttons + three rotary encoders for
switching hosts, controlling Home Assistant, displaying live status, and
sending HID input to whichever host is currently active.

```
                              ┌───────────────────────────┐
                              │   Ajazz AKP03E LCD pad    │   ← user
                              │   6 keys + 3 encoders     │
                              └────────────┬──────────────┘
                                           │ USB host
                              ┌────────────┴──────────────┐
                              │   ESP32-P4-NANO           │   ← deskpad-host
                              │  (Waveshare board)        │
                              │  · DDC/CI to U38 + U24    │──→ U3823DW (KVM)
                              │  · Ethernet + Web UI      │──→ U2419H (secondary)
                              │  · MQTT (HA + discovery)  │
                              │  · UART link ↓            │
                              └───────────┬───────────────┘
                                          │ UART 1 Mbps
                              ┌───────────┴───────────────┐
                              │   Xiao RP2350             │   ← deskpad-hid
       USB-C                  │  · HID keyboard           │
   to active host  ◄──────────┤  · HID consumer-control   │
   (via U38 hub)              │  · USB-CDC log surface    │
                              └───────────────────────────┘
```

## Architecture in one minute

- **The U38 is the KVM.** Its built-in USB hub routes the keyboard/mouse/
  peripherals between hosts when its DisplayPort/USB-C input changes.
  deskpad doesn't move USB; it tells the U38 what to do over DDC/CI.
- **deskpad coordinates the desk.** Reacts to U38 input changes (so the
  secondary monitor follows), exposes the AKP03E keys as a Stream-Deck-
  style surface, and integrates with Home Assistant via MQTT.
- **The HID bridge** (RP2350) is how deskpad sends keystrokes to whichever
  host the U38 has selected — it sits on the U38's USB hub like any
  other peripheral and gets re-attached automatically on switch.

See [CONTEXT.md](CONTEXT.md) for the full glossary and
[docs/adr/](docs/adr/) for architectural decisions.

## Repository layout

| Folder | Board | Role |
|---|---|---|
| [deskpad-host/](deskpad-host/) | Waveshare ESP32-P4-NANO | Main firmware — AKP driver, DDC, web UI, MQTT, HA integration |
| [deskpad-hid/](deskpad-hid/)   | Seeed Xiao RP2350       | USB-HID bridge — receives reports over UART, emits to active host |

## Features

- **KVM coordination.** Switch U38 input via DDC; secondary monitor (U24)
  follows. Detects external switches via RP2350 USB re-enumeration + a
  slow DDC poll fallback.
- **Configurable bindings.** Each of three pages × six LCD keys has a
  renderer (static JPEG, clock, bulb, monitor, thermostat, text value)
  and a binding (KVM select/toggle, HID chord/consumer-control, HA
  service call, raw DDC write).
- **Hybrid scope.** Bindings can be `global` (rendered on every Host)
  or `host:PC1` / `host:PC2` (rendered only when that host is active).
- **State-reactive renderers.** Monitor icon brightens to indicate the
  active Host. Bulb renderer reflects HA entity state subscribed via
  MQTT. (More renderers will come live as MQTT topics are wired up.)
- **Home Assistant integration.** MQTT discovery publishes deskpad as
  an HA device with sensors (active host, current page, link/USB/DDC
  health). Every button press publishes an event topic for HA
  automations. Service-call dispatch via a single `deskpad/cmd` intent
  topic + one HA automation.
- **Notifications.** MQTT-triggered overlays on the AKP — doorbell,
  motion, etc. — with takeover or single-slot styles and configurable
  dismissal.
- **Web UI.** Self-contained SPA at `http://deskpad.local/` for editing
  bindings, encoders, KVM topology, notifications, and credentials,
  plus a dashboard and OTA upload.
- **Public HTTP API.** Every UI action is also a stable JSON endpoint;
  see [docs/api.md](docs/api.md).

## Wiring

### deskpad-host (ESP32-P4-NANO)

| Signal | GPIO | To |
|---|---|---|
| DDC/CI bus A (SDA/SCL) | 4 / 5 | U3823DW (level shifter required) |
| DDC/CI bus B (SDA/SCL) | 6 / 7 | U2419H |
| UART1 TX (to RP2350 RX) | 8 | RP2350 D7 (GP1) |
| UART1 RX (from RP2350 TX) | 9 | RP2350 D6 (GP0) |
| Ethernet RMII (auto) | (board) | RJ45 jack onboard |
| AKP03E USB host | (USB-C) | AKP03E via USB-A→USB-C adapter |

Common GND between P4 and RP2350 is mandatory.

### deskpad-hid (Xiao RP2350)

| Signal | GPIO | To |
|---|---|---|
| UART0 TX (D6 / GP0) | 0 | P4 GPIO 9 |
| UART0 RX (D7 / GP1) | 1 | P4 GPIO 8 |
| USB-C (device) | — | Active host (via U38's USB hub) |

## First-time bring-up

1. **Flash deskpad-host:** open `deskpad-host/` in the ESP-IDF VS Code
   extension. Set target `esp32p4`. Build + flash.
2. **Flash deskpad-hid:** open `deskpad-hid/` in PlatformIO. Build +
   upload (hold BOOTSEL on first flash; UF2 reset thereafter).
3. **Plug in Ethernet.** Console should print `network: got IP …` and
   `mqtt: no broker configured` (expected first boot).
4. **Browse to `http://deskpad.local/`** — the dashboard shows current
   state and lets you configure pages, credentials, notifications, and
   firmware OTA.
5. **Configure MQTT** in the Credentials tab if you want HA integration.
6. **Edit pages 0-2** to bind keys to KVM toggles, HA services, HID
   chords, etc.

## Configuration

NVS-backed JSON. Stamped with defaults on first boot. See the schema
preview at [docs/config-sample.yaml](docs/config-sample.yaml).

Secrets (MQTT host/user/password) live in a separate NVS key and are
never serialised to the main config. Edit via the Credentials tab.

## Architectural decisions

- [ADR-0001](docs/adr/0001-u38-is-the-kvm.md) — U38 is the KVM, deskpad coordinates it.
- [ADR-0002](docs/adr/0002-host-is-kvm-slot-not-machine.md) — Hosts are KVM slots, not Machines.
- [ADR-0003](docs/adr/0003-hybrid-binding-scope.md) — Hybrid binding scope (global vs host:<id>).
- [ADR-0004](docs/adr/0004-ethernet-no-wifi.md) — Ethernet, not WiFi.

## Roadmap

See [plan.md](plan.md). Major phases through Phase 9 are landed in
firmware; integration testing waits on the physical UART link and DDC
wiring to be completed. Open polish work (live-rebind, press-animation
for live renderers, image upload for static_jpeg, optional Web UI auth)
is tracked there too.
