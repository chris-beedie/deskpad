# KVM Switcher

A two-board DIY KVM that swaps two monitors (DDC/CI) and a keyboard between
two host computers, with WiFi + Web UI + Home Assistant integration.

```
                          USB keyboard
                               │
                               │ USB-A → USB-C OTG adapter
                               ▼
                         ┌──────────────────────┐
                         │  kvm-switcher-       │
                         │  control             │     I2C (level-shifted)
                         │  (Xiao ESP32-S3)     ├───► Monitor A (DDC/CI)
                         │                      │
                         │  · USB host (OTG)    ├───► Monitor B (DDC/CI)
                         │  · WiFi / Web UI     │
                         │  · MQTT (HA disco.)  │
                         │  · Hotkey filter     │
                         └─────┬─────────┬──────┘
                               │ UART    ▲ 5V / GND
                               │ link    │ (RP2350 → ESP32)
                               ▼         │
                         ┌─────┴─────────┴──────┐
                         │  kvm-switcher-hid    │
              USB-C      │  (Xiao RP2350)       │
       Host ◄────────────┤  · USB HID kbd       │
       (PC, hub,         │  · USB HID consumer  │
        or dock)         │  · USB-CDC (debug)   │
                         └──────────────────────┘
```

## Repository layout

| Folder | Board | Role |
|---|---|---|
| [kvm-switcher-control/](kvm-switcher-control/) | Seeed Xiao ESP32-S3 | USB host for the keyboard, monitor switching, web/MQTT control |
| [kvm-switcher-hid/](kvm-switcher-hid/) | Seeed Xiao RP2350 | USB device to the PC: HID keyboard + consumer-control + CDC log |

## Wiring

| Signal | Xiao ESP32-S3 | Xiao RP2350 |
|---|---|---|
| Link UART TX | D6 (GPIO43) | D7 (GP1) — RX |
| Link UART RX | D7 (GPIO44) | D6 (GP0) — TX |
| ESP32 hardware reset | RST pad (back of board, near USB-C) | D5 (GP7) |
| 5V rail | 5V (input) | 5V/VBUS (output) |
| Ground | GND | GND |

The **ESP32 hardware reset** wire is required for OTA updates to recover the
keyboard cleanly. After flashing, the firmware asks the RP2350 to briefly
drive this line low, hardware-resetting the ESP32. This is the only reliable
way to reset the USB-OTG analog PHY — without it, an OTA-triggered software
reset leaves the keyboard in a stale enumerated state and you have to
physically replug it. The line is held high-Z by the RP2350 except during
the ~20 ms reset pulse, so the ESP32's RESET button still works normally.

Plus on the ESP32-S3 only:

| Function | Pin |
|---|---|
| I2C SDA / SCL → Monitor A (Wire) | D0 / D1 (GPIO1 / GPIO2) |
| I2C SDA / SCL → Monitor B (Wire1) | D2 / D3 (GPIO3 / GPIO4) |
| KVM input button (to GND) | D4 (GPIO5) |
| Status NeoPixel | D5 (GPIO6) |
| Debug button | BOOT (GPIO0) |
| USB host (D+/D-) | USB-C (GPIO19/20) |

## Power topology

```
                     5V                  5V (5V pin →           5V (USB-C VBUS,
                  via VBUS               5V pin, wired)         host mode)
Host USB-C  ───────────────►  RP2350  ──────────────────►  ESP32-S3  ──────────────►  Keyboard
(PC, hub,                                                                           (USB-A via OTG
 or dock)                                                                            adapter)
```

The "Host USB-C" upstream of the RP2350 is whatever provides 5V + USB data —
a PC port, a powered hub, or a docking station all work. A direct USB 2.0 PC
port (500 mA) is enough on paper for RP2350 (~30 mA) + ESP32-S3 (~150 mA peak
with WiFi) + a typical wired keyboard (~50 mA), but a powered hub or dock
gives you headroom and keeps the device powered when the host PC is off,
which is convenient for the captive-portal / always-on Web UI use case.

**Don't have both boards plugged into separate USB-C cables while the 5V
rail between them is wired** — pull one host cable before joining 5V.

## USB host on the ESP32-S3

The Xiao ESP32-S3's USB-C is wired as a device (Rd pull-downs on CC). To use
it as a host, plug in a **USB-C → USB-A OTG adapter** and then a USB-A
keyboard. A plain USB-C-to-USB-C cable will not enumerate.

## Inter-board protocol

UART, 1 000 000 baud, 8N1, full-duplex, simple framed packets:

```
0xAB | type | len | payload[len] | xor(type, len, payload)
```

| Type | Direction | Payload |
|---|---|---|
| `0x01` KEYBOARD | ESP32 → RP2350 | 8-byte boot keyboard report |
| `0x02` CONSUMER | ESP32 → RP2350 | 2-byte LE consumer-control usage code |
| `0x10` LOG | ESP32 → RP2350 | UTF-8 log line (≤ 220 bytes) |
| `0x20` USB_STATUS | RP2350 → ESP32 | 1 byte: bit0 mounted, bit1 suspended |
| `0x21` HEARTBEAT | RP2350 → ESP32 | empty — sent every 1 s |

The ESP32 considers the link **down** if no heartbeat arrives for 3 s. Link
state is exposed via the Web UI status JSON (`hid_link_up`, `usb_mounted`,
`usb_suspended`) and republished over MQTT on every transition.

## First-time bring-up

1. **Flash each board in isolation** (own USB-C cable to your computer).
2. **RP2350 first flash**: hold `BOOTSEL` while plugging in → mounts as a UF2
   drive. Run `pio run -e xiao_rp2350 -t upload`. After this initial flash
   subsequent uploads use the TinyUSB reset interface — no button press.
3. **ESP32-S3 first flash**: hold `BOOT`, tap `RESET`, release `BOOT` →
   ROM bootloader. Run `pio run -e xiao_esp32s3 -t upload`. Subsequent
   uploads trigger the bootloader automatically.
4. **Verify RP2350 stand-alone**: opens as `KVM HID Bridge` in your OS, with
   both an HID keyboard and a CDC serial port. The CDC port should print
   `[USB] mounted=1 suspended=0` on connect. NeoPixel will be solid red
   (no link heartbeat — expected when stand-alone).
5. **Verify ESP32-S3 stand-alone**: on first boot the device has no stored
   credentials and starts an open AP `KVM-Switcher-Setup` (LED solid blue).
   Connect to it from a phone, the captive portal pops up at
   `http://192.168.4.1`, pick your WiFi and (optionally) enter MQTT host /
   port / user / pass, then save. The device reboots, joins your WiFi, and
   the Web UI is reachable at `http://kvm-switcher.local/`. Logs appear
   over the link UART (1 Mbps on D6/D7), so to see them stand-alone you
   need a USB-UART adapter on those pins or to pair it with the RP2350
   first.
6. **Wire the link**: cross TX↔RX, common GND, then add the 5V rail with
   only one of the boards plugged into a USB host. Logs from the ESP32
   now stream out the RP2350's USB-CDC port, prefixed `[ESP] …`.

## Building

```bash
cd kvm-switcher-control && pio run -t upload   # ESP32-S3
cd kvm-switcher-hid     && pio run -t upload   # RP2350
```

## Configuration

There is no source-tree config file — credentials live in NVS on the device.

**First boot / no stored WiFi:** the ESP32 comes up as an open AP
`KVM-Switcher-Setup` with a captive portal at `http://192.168.4.1`. The portal
form takes the WiFi SSID + password and (optionally) MQTT host / port / user /
password. Save and the device reboots into station mode. The status LED is
solid blue while the portal is active.

**Once joined:** everything else is editable from the Web UI under the
*MQTT settings* and *Hotkey settings* drop-downs, and persisted in NVS.

**Forgetting WiFi (e.g. moving to a new network):** either click **Forget WiFi
& reboot** in the *WiFi* drop-down on the Web UI, or **hold the KVM input
button for 10 seconds** (LED previews red at 3 s = reboot, purple at 10 s =
factory reset; release inside the colour you want). This wipes WiFi creds
and MQTT settings, then reboots into the captive portal.

## OTA updates (ESP32 only)

The control board uses `default_8MB.csv` partitioning: two 3.3 MB OTA app
slots plus a 1.4 MB SPIFFS region. ElegantOTA is mounted at `/update` by
the AsyncWebServer.

Upload a new firmware over the air:

1. Build but don't flash: `pio run -e xiao_esp32s3` produces
   `.pio/build/xiao_esp32s3/firmware.bin`.
2. Browse to `http://kvm-switcher.local/update` (or the device's IP),
   select **Firmware**, pick the `.bin`, and upload.
3. The device verifies the image, swaps OTA slots, and reboots. On first
   boot the new image must call `esp_ota_mark_app_valid_cancel_rollback()`
   (already done in `setup()`); otherwise the bootloader rolls back.

The RP2350 has no OTA — flash it via UF2 mode (hold BOOTSEL while
plugging USB-C).

### Re-flashing the ESP32 over USB (when OTA isn't an option)

The ESP32 only has one USB-C port and it's normally hosting the keyboard.
To USB-flash it once everything is wired up:

1. **Unplug the keyboard's OTG adapter** from the ESP32's USB-C port — that
   port is your upload cable port.
2. **Unplug the RP2350's USB-C cable from your PC** (or cut the 5V tie
   between the two boards). Otherwise both boards' VBUS rails are joined
   while both USB-C cables go to PCs, which backfeeds one PC's USB into
   the other.
3. The UART link wires (D6 ↔ D7, GND) and the 5V tie can stay if the
   RP2350 is unplugged from its host — the RP2350 will simply run from
   the ESP32's USB power while you flash.
4. **Enter download mode manually**: hold `BOOT`, tap `RESET`, release
   `BOOT`. This is required after the first flash because
   `ARDUINO_USB_MODE=0` (USB host mode) means the chip is *not* visible
   as a serial device while the firmware runs — esptool can't trigger
   the auto-reset.
5. Run `pio run -e xiao_esp32s3 -t upload`.
6. Tap `RESET` (or it'll restart automatically) to run the new firmware.
