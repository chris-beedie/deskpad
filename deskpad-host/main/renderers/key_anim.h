#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Owns the LCD page state and pushes JPEGs to the AKP03E from a worker task.
//
// Why a worker: the AKP IN callback runs on the USB client task, and
// akp03e_send_out() blocks waiting for an OUT completion that fires on the
// same task — calling it directly from the IN callback deadlocks.

#define KEY_ANIM_PAGE_COUNT 3
#define KEY_ANIM_KEY_COUNT  6

esp_err_t key_anim_init(void);

// Switch to a page and redraw all 6 keys with that page's default images.
// Out-of-range pages are clamped.
void    key_anim_set_page(uint8_t page);
uint8_t key_anim_get_page(void);

// Queue the default / pressed image for one key on the current page.
// No-op when the slot is bound to a live key (live_key owns the visual).
void key_anim_show_default(uint8_t key);
void key_anim_show_pressed(uint8_t key);

// Live-slot registration. Bound by live_key on creation; on a page switch
// to the slot's page, on_show(user) fires so the live renderer can refresh.
typedef void (*key_anim_on_show_cb_t)(void *user);
void key_anim_bind_live(uint8_t page, uint8_t key,
                        key_anim_on_show_cb_t on_show, void *user);
bool key_anim_is_live(uint8_t page, uint8_t key);

// Overlay layer — takes priority over the live binding for `key`.
// Used by notifications and the boot-time health surface to temporarily
// replace a slot's content. Clearing triggers a re-render via the
// underlying live binding (if any) or the embedded JPEG.
//
// To take over all slots on the current page, set the overlay on each
// (current_page, k) for k = 0..5.
void key_anim_set_overlay(uint8_t page, uint8_t key,
                          key_anim_on_show_cb_t on_show, void *user);
void key_anim_clear_overlay(uint8_t page, uint8_t key);
