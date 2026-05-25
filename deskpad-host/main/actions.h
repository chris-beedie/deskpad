#pragma once

#include "akp03e.h"
#include "config.h"
#include "esp_err.h"

// Translate an AKP03E input event into a KVM action and execute it.
// All slow work (DDC, MQTT publish, etc.) runs on a worker task to avoid
// blocking USB callbacks.

// Create the dispatch queue + worker task. Must be called before any events.
esp_err_t actions_init(void);

// Non-blocking — enqueues the event for the worker. Safe to call from the
// USB client task / interrupt callback context.
void actions_handle(const akp03e_event_t *ev);

// Public binding execution — used by /api/action/* endpoints to trigger
// actions from external clients. The slot context is optional; pass NULL
// when no per-slot entity inheritance is needed (the caller must put any
// required fields directly on the binding). Runs synchronously on the
// caller's task.
void actions_fire(const binding_t *b, const slot_config_t *slot);

// AKP backlight brightness, 0..100. Persisted to NVS so the value
// survives reboots. _set() also pushes the new value to the AKP
// immediately if it's connected.
uint8_t actions_brightness_get(void);
void    actions_brightness_set(uint8_t percent);
