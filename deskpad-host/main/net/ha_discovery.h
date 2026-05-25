#pragma once

#include <stdint.h>
#include "esp_err.h"

// Publishes deskpad to Home Assistant via MQTT discovery:
//   - Read-only sensors:
//       active_host    "PC1" | "PC2" | "unknown"
//       current_page   "0" | "1" | "2"
//       hid_link       "up" | "down"
//       usb_mounted    "true" | "false"
//       ddc_a / ddc_b  "ok" | "fail"
//   - Event topics on every input press (user wires HA MQTT triggers):
//       deskpad/event/press/p<page>s<slot>   payload "press"
//       deskpad/event/press/side<n>          payload "press"
//       deskpad/event/encoder/<n>/press      payload "press"
//       deskpad/event/encoder/<n>/twist      payload "+1" or "-1"
//
// Must run after mqtt_init(). Re-publishes discovery on every MQTT
// reconnect (retained, so HA dedupes).

esp_err_t ha_discovery_init(void);

// Publish a press event. Called from actions.c when a button or
// encoder event is enqueued.
void ha_publish_press_slot(uint8_t page, uint8_t slot);
void ha_publish_press_side(uint8_t which);                   // 0..2
void ha_publish_press_encoder(uint8_t encoder);              // 0..2
void ha_publish_twist_encoder(uint8_t encoder, int8_t dir);  // +1 / -1
