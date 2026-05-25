#pragma once

#include "esp_err.h"

// `bulb` renderer — visually reflects an HA entity's on/off state.
// Filled bulb icon when "on", outline-only when "off", dim when state
// is unknown (no message received yet).
//
// State source per slot:
//   - explicit `state_topic` in display config (highest precedence)
//   - else derived from slot.entity:  homeassistant/<domain>/<id>/state
//   - else slot is rendered as "state unknown"
//
// On-value match: from display.on_value if set, otherwise "on".
//
// Must run after live_key_init(), config_init(), and mqtt_init().

esp_err_t bulb_render_init(void);
