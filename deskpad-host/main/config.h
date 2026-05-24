#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "akp03e.h"
#include "key_anim.h"

// Phase 1 binding config: persisted in NVS as a JSON blob, mirrors the
// hardcoded action tables that used to live in actions.c.
//
// Schema (version 1):
//   {
//     "version": 1,
//     "pages": [
//       { "slots": ["KVM_PC1","KVM_PC2",...] },   // 3 entries, 6 slots each
//       ...
//     ],
//     "encoders": [
//       { "press": "MUTE", "twist_pos": "VOL_UP", "twist_neg": "VOL_DOWN" },
//       ...                                       // 3 entries
//     ]
//   }
//
// Side keys (indices 6/7/8) are deliberately not in the config — they are
// hard-wired page switchers per the deskpad spec.

#define CONFIG_SCHEMA_VERSION 1

typedef enum {
    A_NONE = 0,

    A_PAGE_0, A_PAGE_1, A_PAGE_2,

    A_KVM_PC1, A_KVM_PC2, A_KVM_TOGGLE,
    A_MON_A_IN1, A_MON_A_IN2,
    A_MON_B_IN1, A_MON_B_IN2,

    A_PLAY_PAUSE, A_PREV_TRACK, A_NEXT_TRACK,
    A_VOL_UP, A_VOL_DOWN, A_MUTE,

    A_LOCK, A_SLEEP, A_SCREENSHOT, A_REC, A_DND, A_CALC,

    A_BRIGHT_A_UP, A_BRIGHT_A_DOWN, A_BRIGHT_B_UP, A_BRIGHT_B_DOWN,
} action_t;

typedef struct {
    action_t slots[KEY_ANIM_KEY_COUNT];
} config_page_t;

typedef struct {
    action_t press;
    action_t twist_pos;
    action_t twist_neg;
} config_encoder_t;

typedef struct {
    uint32_t version;
    config_page_t pages[KEY_ANIM_PAGE_COUNT];
    config_encoder_t encoders[AKP03E_ENCODER_COUNT];
} config_t;

// Load config from NVS, stamping defaults if absent or schema-mismatched.
// Must run before actions_init().
esp_err_t config_init(void);

// Pointer-stable handle to the current config. The pointer never changes
// across the lifetime of the process; the contents are updated in place
// by config_set_from_json(). Callers should treat reads as racy-but-
// atomic (each field is a simple enum / scalar; tearing is harmless).
const config_t *config_get(void);

// Replace the current config with the result of parsing `json`, then
// persist to NVS. Returns ESP_ERR_INVALID_ARG on parse failure or schema
// mismatch — in that case the in-memory config is unchanged.
esp_err_t config_set_from_json(const char *json);

// Serialise the current config to a freshly-malloced JSON string. Caller
// owns the result and must free() it. Returns NULL on allocation failure.
char *config_to_json(void);

// Name <-> enum lookup. Unknown name -> A_NONE. Unknown enum -> "NONE".
action_t    action_from_name(const char *name);
const char *action_name(action_t a);
