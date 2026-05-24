#pragma once

#include "akp03e.h"
#include "esp_err.h"

// Translate an AKP03E input event into a KVM action and execute it.
// Today the executors are stubs that just log; later they'll drive DDC/CI,
// the HID bridge, MQTT, etc.

// Create the dispatch queue + worker task. Must be called before any events.
esp_err_t actions_init(void);

// Non-blocking — enqueues the event for the worker. Safe to call from the
// USB client task / interrupt callback context: DDC/CI and other slow
// executors run on the worker, not here.
void actions_handle(const akp03e_event_t *ev);
