#pragma once

#include <stdbool.h>
#include "esp_err.h"

// Sensitive configuration — MQTT credentials and any future
// host-agent tokens. Stored in NVS under a SEPARATE key from
// the main config, never returned by /api/config, write-only
// for sensitive fields (password GET returns "has_password"
// flag rather than the value).

#define SECRETS_MQTT_HOST_MAX 64
#define SECRETS_MQTT_USER_MAX 64
#define SECRETS_MQTT_PASS_MAX 64

typedef struct {
    char     mqtt_host[SECRETS_MQTT_HOST_MAX];
    uint16_t mqtt_port;
    char     mqtt_user[SECRETS_MQTT_USER_MAX];
    char     mqtt_pass[SECRETS_MQTT_PASS_MAX];   // empty when not set
} secrets_t;

// Load from NVS. Missing entries become empty strings / port 0.
esp_err_t secrets_init(void);

// Pointer to in-memory secrets struct. Do not mutate directly;
// use secrets_set_mqtt() for atomic update + persist.
const secrets_t *secrets_get(void);

// Update MQTT creds. Pass NULL for any field you want to leave
// unchanged. Empty string clears the field. Returns ESP_OK if
// persisted to NVS.
esp_err_t secrets_set_mqtt(const char *host, const uint16_t *port,
                            const char *user, const char *password);

// JSON for /api/credentials GET — password masked as
// {"has_password": true|false}. Caller frees the returned string.
char *secrets_to_masked_json(void);
