# deskpad HTTP API

All endpoints under `http://deskpad.local/api/`. JSON in / JSON out
unless otherwise noted. No auth in the current build (LAN-only trust
model — Phase 9 may add a PIN). The SPA at `/` is one client of this
API; you can call any of it from `curl`, Home Assistant, scripts, etc.

## State / observability

### `GET /api/state`
Runtime snapshot.
```json
{
  "active_host": "PC1" | "PC2" | "unknown",
  "current_page": 0,
  "hid_link_up": true,
  "usb_mounted": true,
  "usb_suspended": false,
  "mqtt_connected": true,
  "notification_active": false,
  "uptime_s": 1234,
  "ip": "10.0.20.27"
}
```

### `GET /api/health`
Subsystem-status registry — every component reports OK / WARN / ERROR
with a one-line message.
```json
{
  "worst": "OK",
  "entries": [
    { "id": "ddc_a",   "severity": "OK",    "message": "ready" },
    { "id": "ddc_b",   "severity": "OK",    "message": "ready" },
    { "id": "network", "severity": "OK",    "message": "10.0.20.27" },
    { "id": "hid_link","severity": "OK",    "message": "link up" },
    { "id": "mqtt",    "severity": "OK",    "message": "connected" }
  ]
}
```

### `GET /api/enums`
Vocabularies the SPA uses for dropdowns.
```json
{
  "scopes":        ["global", "host:PC1", "host:PC2"],
  "hosts":         ["PC1", "PC2"],
  "renderers":     ["none", "static_jpeg", "clock", "bulb", "monitor",
                    "thermostat", "text_value"],
  "binding_types": ["none", "kvm_select", "kvm_toggle", "hid_chord",
                    "hid_consumer", "ha", "ddc"]
}
```

## Triggers — `POST /api/action/*`

Trigger an action directly. Bodies are JSON.

### `POST /api/action/kvm/toggle`
Flip PC1 ↔ PC2.
```
curl -X POST http://deskpad.local/api/action/kvm/toggle
```

### `POST /api/action/kvm/select`  body: `{"target":"PC1"|"PC2"}`
Switch directly to the named host.

### `POST /api/action/page`  body: `{"page":0..2}`
Switch the AKP to a specific page.

### `POST /api/action/ddc`  body: `{"bus":0|1,"vcp":0..255,"value":0..255}`
Raw DDC write. `bus` 0 = A (U38), 1 = B (U24). Useful for discovering
vendor-area VCPs (e.g. PIP control on the U38).

### `POST /api/action/hid/chord`  body: `{"chord":"ctrl+shift+m"}`
Send a key chord via the HID bridge. Modifier aliases: `ctrl=lctrl`,
`shift=lshift`, `alt=lalt`, `win=cmd=meta=lwin`. Right-hand variants
explicit (`rctrl` etc.).

### `POST /api/action/hid/consumer`  body: `{"name":"PLAY_PAUSE"}`
Send a consumer-control HID usage. Known names: `PLAY_PAUSE`, `PLAY`,
`PAUSE`, `STOP`, `NEXT_TRACK`, `PREV_TRACK`, `FAST_FWD`, `REWIND`,
`VOL_UP`, `VOL_DOWN`, `MUTE`, `EJECT`.

## Config

### `GET /api/config`
Whole binding configuration as JSON. Schema v2 — see
[docs/config-sample.yaml](config-sample.yaml).

### `PUT /api/config`
Replace the whole config. Body must be a complete v2 document.
Persisted to NVS.

> Note: changes apply on next boot for most renderers — live rebind
> (no-reboot) is planned but not yet implemented for all renderers.
> KVM topology, binding dispatch, and scope filtering all do apply
> immediately.

## Credentials

### `GET /api/credentials`
Masked snapshot of stored secrets.
```json
{ "mqtt": { "host": "ha.local", "port": 1883, "user": "deskpad", "has_password": true } }
```

### `PUT /api/credentials`
Update credentials. Any field omitted is left unchanged. `password: ""`
clears it. On save, MQTT reconnects.
```json
{ "mqtt": { "host": "ha.local", "port": 1883, "user": "deskpad", "password": "secret" } }
```

## Notifications

### `GET /api/notifications`
List of configured MQTT-triggered overlays.
```json
[
  {
    "id": "doorbell", "priority": 10,
    "trigger": { "topic": "homeassistant/binary_sensor/doorbell/state", "on_value": "on" },
    "display": {
      "style": "takeover", "target_slot": 0, "label": "DOORBELL",
      "colour": "#E24B4A", "duration_ms": 30000,
      "dismiss_any_key": true, "dismiss_source_off": true
    }
  }
]
```

### `PUT /api/notifications`
Replace the configured set. Body is an array of notification objects in
the same shape. Up to 8 notifications supported.

## Firmware

### `POST /api/ota`
Body: raw `.bin` of the new firmware. Streams into the inactive OTA
slot; on `200 OK` the new image is staged. Follow with `/api/reboot`.

### `POST /api/reboot`
Schedules `esp_restart()` ~500 ms in the future so the response reaches
the client first.

## MQTT topics deskpad publishes

| Topic | Direction | Payload | Notes |
|---|---|---|---|
| `homeassistant/sensor/deskpad/<id>/config` | publish (retain) | discovery JSON | one per sensor, on connect |
| `deskpad/state/active_host`   | publish (retain) | `PC1` / `PC2` / `unknown` | |
| `deskpad/state/current_page`  | publish (retain) | `0` / `1` / `2` | |
| `deskpad/state/hid_link`      | publish (retain) | `up` / `down` | |
| `deskpad/state/usb_mounted`   | publish (retain) | `true` / `false` | |
| `deskpad/state/ddc_a`         | publish (retain) | `ok` / `fail` | |
| `deskpad/state/ddc_b`         | publish (retain) | `ok` / `fail` | |
| `deskpad/event/press/p<N>s<M>` | publish | `press` | LCD key press |
| `deskpad/event/press/side<N>` | publish | `press` | side key press |
| `deskpad/event/encoder/<N>/press` | publish | `press` | encoder press |
| `deskpad/event/encoder/<N>/twist` | publish | `+1` / `-1` | encoder twist |
| `deskpad/cmd` | publish | `{"service":"...","entity":"..."}` | HA service-call intents (your HA automation dispatches) |

## Example HA automation — dispatch deskpad service calls

```yaml
automation:
  - alias: "Deskpad service dispatch"
    trigger:
      - platform: mqtt
        topic: deskpad/cmd
    action:
      - service: "{{ trigger.payload_json.service }}"
        data:
          entity_id: "{{ trigger.payload_json.entity }}"
```

## Example — trigger from curl

```bash
# toggle KVM
curl -X POST http://deskpad.local/api/action/kvm/toggle

# play / pause music on the active host
curl -X POST http://deskpad.local/api/action/hid/consumer \
  -H 'content-type: application/json' \
  -d '{"name":"PLAY_PAUSE"}'

# set U38 brightness to 60
curl -X POST http://deskpad.local/api/action/ddc \
  -H 'content-type: application/json' \
  -d '{"bus":0,"vcp":16,"value":60}'

# snapshot the device's state
curl http://deskpad.local/api/state | jq
```
