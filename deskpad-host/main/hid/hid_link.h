#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// UART link from deskpad-host (ESP32-P4-NANO) to deskpad-hid (Xiao
// RP2350). The RP2350 acts as a USB-HID device facing the host PC;
// this link is the channel by which the P4 tells it which reports to
// emit.
//
// Wire protocol (matches deskpad-hid/src/hid_link.h):
//   0xAB | type | len | payload[len] | xor(type, len, payload)
//   Baud 1 Mbps, 8N1, full-duplex.
//
// Default pins on the P4-NANO side:
//   GPIO 8 = TX -> RP2350 GP1 (RX)
//   GPIO 9 = RX <- RP2350 GP0 (TX)
//   Common GND between boards required.
//
// hid_link_init() picks up these defaults; the caller may override
// pins by passing -1 for default or specific GPIOs.

#define HID_LINK_DEFAULT_TX_GPIO   8
#define HID_LINK_DEFAULT_RX_GPIO   9
#define HID_LINK_BAUD              1000000

// Init the UART, install RX task. Safe to call once. Pass -1 to use the
// HID_LINK_DEFAULT_*_GPIO above.
esp_err_t hid_link_init(int tx_gpio, int rx_gpio);

// Send an 8-byte boot-keyboard report:
//   byte 0: modifier bitmask (LCTRL=0x01, LSHIFT=0x02, LALT=0x04, LWIN=0x08,
//                              RCTRL=0x10, RSHIFT=0x20, RALT=0x40, RWIN=0x80)
//   byte 1: reserved (0)
//   byte 2..7: up to 6 simultaneous HID usage IDs (0 = empty)
esp_err_t hid_link_send_keyboard(uint8_t modifiers, const uint8_t keys[6]);

// Convenience: send a key-down for a single chord, then immediately
// send a release report. For tap-style bindings.
esp_err_t hid_link_send_chord_tap(uint8_t modifiers, uint8_t keycode);

// Send a 16-bit consumer-control usage code (e.g. 0xCD play/pause).
// Tap-style: emits the code then a 0x0000 release.
esp_err_t hid_link_send_consumer_tap(uint16_t usage);

// Send a 16-bit system-control usage code (0x81 power down, 0x82 sleep,
// 0x83 wake). Tap-style as above.
esp_err_t hid_link_send_system_tap(uint16_t usage);

// True if the RP2350 has heartbeat'd within the last 3 s.
bool     hid_link_is_up(void);

// Latest USB-to-PC status reported by the RP2350.
bool     hid_link_usb_mounted(void);
bool     hid_link_usb_suspended(void);

// Callback fired from the hid_link RX task whenever a USB status frame
// arrives. Used by kvm_detect to notice external KVM switches (the
// RP2350's USB-to-PC re-enumerates when the U38 swaps input). Keep the
// callback cheap; defer work to your own task.
typedef void (*hid_link_usb_status_cb_t)(bool mounted, bool suspended, void *user);
void     hid_link_set_usb_status_cb(hid_link_usb_status_cb_t cb, void *user);
