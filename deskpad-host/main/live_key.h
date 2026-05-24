#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

// A "live key" owns a (page, key) slot whose image is rendered on demand from
// firmware rather than picked from the embedded JPEGs. The render function
// receives an LVGL canvas configured to draw into a 60x60 RGB888 buffer; on
// return, the framework rotates the buffer 90° CW (to compensate for the
// AKP03E's display rotation), JPEG-encodes via the hardware encoder, and
// pushes to the device.
//
// Rendering happens on a single shared worker task off the USB callback
// context — same reason the static-image path uses a worker (see key_anim).

typedef struct live_key_s *live_key_handle_t;
typedef void (*live_key_render_fn_t)(lv_obj_t *canvas, void *user);

esp_err_t live_key_init(void);

// Bind a render function to (page, key). Multiple slots may share a render
// function (with distinct `user` payloads). Returns NULL on failure.
live_key_handle_t live_key_create(uint8_t page, uint8_t key,
                                  live_key_render_fn_t render, void *user);

// Non-blocking — queues a re-render. Safe from any task (including ISRs via
// portYIELD_FROM_ISR caveat; we keep it task-only here).
void live_key_invalidate(live_key_handle_t handle);
