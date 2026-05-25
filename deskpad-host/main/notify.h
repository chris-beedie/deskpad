#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// Notification subsystem — MQTT-triggered overlays on the AKP.
//
// A notification has:
//   - a trigger (MQTT topic + on-value match)
//   - a display style (takeover = all 6 keys; overlay = one slot)
//   - dismissal options (auto-timeout, any-key press, source-cleared)
//   - a priority (higher beats lower; same-priority queues)
//
// Notifications are stored in a separate NVS key from the main config
// (`notify_v1`). Empty by default.

typedef enum {
    NOTIF_STYLE_TAKEOVER = 0,
    NOTIF_STYLE_OVERLAY  = 1,
} notif_style_t;

#define NOTIF_ID_MAX            24
#define NOTIF_TOPIC_MAX         120
#define NOTIF_VALUE_MAX         32
#define NOTIF_LABEL_MAX         40
#define NOTIF_COLOUR_MAX        8
#define NOTIF_MAX               8       // total notifications allowed

typedef struct {
    char          id[NOTIF_ID_MAX];
    int           priority;
    char          trigger_topic[NOTIF_TOPIC_MAX];
    char          trigger_on_value[NOTIF_VALUE_MAX];
    notif_style_t style;
    uint8_t       target_slot;            // for OVERLAY style
    char          label[NOTIF_LABEL_MAX];
    char          colour_hex[NOTIF_COLOUR_MAX];   // "#RRGGBB"
    int           duration_ms;            // 0 = no auto-expire
    bool          dismiss_any_key;
    bool          dismiss_source_off;
} notification_def_t;

// Init from NVS (or stamp empty default). Must run after mqtt_init().
esp_err_t notify_init(void);

// Snapshot for the SPA / external API.
size_t    notify_get_all(notification_def_t *out, size_t max);

// Replace the configured set + persist + resubscribe MQTT triggers.
esp_err_t notify_set_all(const notification_def_t *defs, size_t n);

// Called from actions.c on slot press. Returns true if an active
// notification consumed the press (caller must NOT dispatch the
// slot's binding).
bool      notify_consume_press(uint8_t slot);

// True if an overlay is currently shown.
bool      notify_overlay_active(void);
