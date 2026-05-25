#pragma once

#include <stdbool.h>
#include <stdint.h>

// Parse a chord string like "ctrl+shift+m" or "win+l" into a modifier
// bitmask + single HID keycode. Case-insensitive. Returns true on success;
// false if no recognised keycode was found.
//
// Modifier aliases:
//   ctrl  = lctrl,  shift = lshift,  alt   = lalt,  win/cmd/meta = lwin
//   rctrl, rshift, ralt, rwin are explicit
bool hid_keys_parse_chord(const char *str, uint8_t *modifiers, uint8_t *keycode);

// Look up a consumer-control usage code by name (e.g. "PLAY_PAUSE",
// "VOL_UP", "MUTE"). Case-insensitive. Returns 0 if unknown.
uint16_t hid_keys_consumer_code(const char *name);

// Look up a system-control usage code by name (e.g. "POWER_DOWN",
// "SLEEP", "WAKE_UP"). Returns 0 if unknown.
uint16_t hid_keys_system_code(const char *name);
