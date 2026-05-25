#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// Central health registry. Subsystems report their current state via
// system_status_set(id, severity, message). Consumers (the SPA's
// /api/health endpoint, the boot-time AKP overlay, MQTT HA discovery)
// read snapshots and subscribe to changes.

typedef enum {
    STATUS_OK    = 0,
    STATUS_WARN  = 1,
    STATUS_ERROR = 2,
} severity_t;

typedef struct {
    char       id[24];
    severity_t severity;
    char       message[80];
} status_entry_t;

#define MAX_STATUS_ENTRIES        16
#define MAX_STATUS_SUBSCRIBERS    4

esp_err_t system_status_init(void);

// Update an entry. `id` is the stable subsystem name ("ddc_a", "hid_link",
// "mqtt", "network", ...). `fmt + args` formats the message. Idempotent
// when both severity and message are unchanged.
void      system_status_set(const char *id, severity_t sev, const char *fmt, ...);

// Snapshot copy of all entries. Returns count written.
size_t    system_status_get_all(status_entry_t *out, size_t max);

// Worst-case severity across all entries.
severity_t system_status_worst(void);

typedef void (*system_status_cb_t)(const status_entry_t *entry, void *user);
void      system_status_subscribe(system_status_cb_t cb, void *user);

// Helper — "OK" / "WARN" / "ERROR" string for JSON output.
const char *system_status_severity_str(severity_t s);
