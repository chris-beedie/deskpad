#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// MQTT layer with a small subscription registry. Renderers and other
// components register interest in a topic; the dispatcher calls them
// back when messages arrive.
//
// On boot, reads MQTT creds from `secrets` and connects. Reconnects
// automatically. Subscriptions persist across reconnects (re-subscribed
// on CONNECTED event).

typedef void (*mqtt_subscriber_cb_t)(const char *topic,
                                      const char *payload, size_t payload_len,
                                      void *user);

// Init the client. No-op if no MQTT host configured in secrets.
// Idempotent — calling again after secrets change reconnects.
esp_err_t mqtt_init(void);

// Reconnect with current secrets (called when /api/credentials updates).
esp_err_t mqtt_reload(void);

// True if currently connected to broker.
bool      mqtt_is_connected(void);

// Subscribe to a topic. The callback fires for every message on a
// matching topic. Wildcards (`+`, `#`) are passed through to the broker;
// the dispatcher matches by exact topic for local routing, so use
// individual subscriptions per topic for now.
//
// Returns a handle ≥ 0 on success, ≤ 0 on failure. Same handle is used
// for unsubscribe.
int       mqtt_subscribe(const char *topic, mqtt_subscriber_cb_t cb, void *user);
void      mqtt_unsubscribe(int handle);

// Publish a UTF-8 payload to a topic. `retain` enables MQTT's retained-
// message flag (useful for discovery + state publishes that should
// persist).
esp_err_t mqtt_publish(const char *topic, const char *payload, bool retain);
