#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "akp03e.h"
#include "key_anim.h"

// Schema v2 — rich binding model that the YAML mock in docs/config-sample.yaml
// describes. Each slot has a scope, an optional shared entity, a display
// (named renderer + per-renderer config), and a binding (typed + per-type
// params). v1 configs in NVS are migrated forward on boot.

#define CONFIG_SCHEMA_VERSION 2

// ---------------------------------------------------------------------------
// Enums

typedef enum {
    HOST_PC1 = 1,
    HOST_PC2 = 2,
} host_t;

typedef enum {
    SCOPE_GLOBAL = 0,
    SCOPE_HOST_PC1,
    SCOPE_HOST_PC2,
} scope_t;

typedef enum {
    REND_NONE = 0,
    REND_STATIC_JPEG,
    REND_CLOCK,
    REND_BULB,
    REND_MONITOR,
    REND_THERMOSTAT,
    REND_TEXT_VALUE,
} renderer_id_t;

typedef enum {
    BIND_NONE = 0,
    BIND_KVM_SELECT,   // target = HOST_PC1 | HOST_PC2
    BIND_KVM_TOGGLE,   // no params — flips Active Host
    BIND_HID_CHORD,    // hid_chord (e.g. "ctrl+shift+m")
    BIND_HID_CONSUMER, // hid_consumer (e.g. "PLAY_PAUSE", "VOL_UP")
    BIND_HA,           // ha_service (+ ha_entity override or null = use slot.entity)
    BIND_DDC,          // ddc_bus + ddc_vcp + (ddc_delta | ddc_value | ddc_command)
} binding_type_t;

typedef enum {
    DDC_VAR_NONE = 0,
    DDC_VAR_DELTA,    // signed relative change
    DDC_VAR_VALUE,    // absolute 0..255
    DDC_VAR_COMMAND,  // named (e.g. "pip_off") — resolves via firmware table
} ddc_variant_t;

// ---------------------------------------------------------------------------
// Aggregates

typedef struct {
    char    *label;
    // static_jpeg
    char    *image;
    // bulb / text_value (state subscription overrides; null = derive from slot.entity)
    char    *state_topic;
    char    *on_value;        // bulb only
    // monitor
    host_t   lit_when;
    // thermostat
    char    *current_topic;
    char    *target_topic;
    char    *boost_remaining_topic;
    // text_value
    char    *unit;
} renderer_config_t;

typedef struct {
    binding_type_t type;
    // kvm_select (toggle ignores target)
    host_t   kvm_target;
    // hid
    char    *hid_chord;        // chord variant string
    char    *hid_consumer;     // consumer key name
    bool     hid_hold;         // (reserved — RP2350 doesn't support yet)
    // ha
    char    *ha_service;
    char    *ha_entity;        // null = inherit slot.entity
    // ddc
    int      ddc_bus;          // 0 = A, 1 = B
    int      ddc_vcp;          // VCP code (0x00..0xFF)
    ddc_variant_t ddc_variant;
    int      ddc_delta;
    int      ddc_value;
    char    *ddc_command;
} binding_t;

typedef struct {
    scope_t           scope;
    char             *entity;     // optional HA entity (null = none)
    renderer_id_t     renderer;
    renderer_config_t display;
    binding_t         binding;
} slot_config_t;

typedef struct {
    slot_config_t slots[KEY_ANIM_KEY_COUNT];
} page_config_t;

typedef struct {
    binding_t press;
    binding_t twist_pos;
    binding_t twist_neg;
} encoder_page_binding_t;

typedef struct {
    encoder_page_binding_t pages[KEY_ANIM_PAGE_COUNT];
} encoder_config_t;

typedef struct {
    int      ddc_bus;    // 0 = bus A, 1 = bus B (rarely changes)
    uint8_t  input_pc1;  // VCP 0x60 value for PC1
    uint8_t  input_pc2;  // VCP 0x60 value for PC2
} monitor_config_t;

typedef struct {
    monitor_config_t monitor_a;
    monitor_config_t monitor_b;
} kvm_config_t;

typedef struct {
    uint32_t        version;
    kvm_config_t    kvm;
    page_config_t   pages[KEY_ANIM_PAGE_COUNT];
    encoder_config_t encoders[AKP03E_ENCODER_COUNT];
} config_t;

// ---------------------------------------------------------------------------
// API

// Load config from NVS, migrating v1 -> v2 if needed, or stamping defaults
// if absent. Must run before actions_init().
esp_err_t config_init(void);

// Pointer-stable handle to the current config. The pointer never changes
// across the lifetime of the process; the contents are updated in place by
// config_set_from_json(). Treat reads as racy-but-atomic at field level
// (each scalar is a single word; string pointers are atomic).
const config_t *config_get(void);

// Replace the current config with the JSON body, persist to NVS. Returns
// ESP_ERR_INVALID_ARG on parse / schema failure; in-memory config unchanged.
esp_err_t config_set_from_json(const char *json);

// Serialise the current config to a freshly-malloced JSON string. Caller
// owns the result and must free() it.
char *config_to_json(void);

// Convenience name <-> enum helpers (used by /api/actions and by the SPA).
const char  *scope_name(scope_t s);
scope_t      scope_from_name(const char *name);
const char  *host_name(host_t h);
host_t       host_from_name(const char *name);
const char  *renderer_name(renderer_id_t r);
renderer_id_t renderer_from_name(const char *name);
const char  *binding_type_name(binding_type_t t);
binding_type_t binding_type_from_name(const char *name);
