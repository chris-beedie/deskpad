#pragma once

#include "esp_err.h"

// `monitor` renderer — draws a small monitor icon onto an AKP key. The
// screen colour reflects whether the slot's `lit_when` host matches the
// current Active Host.
//
// On init, walks the loaded config and creates a live_key for every slot
// whose display.renderer == REND_MONITOR. Subscribes to host_state so it
// can invalidate (re-render) those keys whenever the Active Host changes.
//
// Reconfiguration (PUT /api/config) doesn't currently rebind these — a
// reboot is required for changes to take effect. Phase 4b polish.
//
// Must run after live_key_init() and config_init().

esp_err_t monitor_render_init(void);
