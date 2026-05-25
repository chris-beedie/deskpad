#pragma once

#include "esp_err.h"

// `clock` renderer — draws current uptime as MM:SS centred on an AKP key.
//
// On init, walks the loaded config and creates a live_key for every slot
// whose display.renderer == REND_CLOCK. A 1 Hz periodic timer invalidates
// all bound clock slots so they re-render once per second.
//
// Reconfiguration (PUT /api/config) doesn't currently rebind — reboot
// required. Phase polish.
//
// Must run after live_key_init() and config_init().

esp_err_t clock_render_init(void);
