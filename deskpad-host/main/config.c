#include "config.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "config";

#define NVS_NAMESPACE  "deskpad"
#define NVS_KEY_CONFIG "config"

// ---------------------------------------------------------------------------
// Action name table — single source of truth for both directions of lookup.

static const struct {
    action_t code;
    const char *name;
} ACTION_NAMES[] = {
    { A_NONE,           "NONE"            },
    { A_PAGE_0,         "PAGE_0"          },
    { A_PAGE_1,         "PAGE_1"          },
    { A_PAGE_2,         "PAGE_2"          },
    { A_KVM_PC1,        "KVM_PC1"         },
    { A_KVM_PC2,        "KVM_PC2"         },
    { A_KVM_TOGGLE,     "KVM_TOGGLE"      },
    { A_MON_A_IN1,      "MON_A_IN1"       },
    { A_MON_A_IN2,      "MON_A_IN2"       },
    { A_MON_B_IN1,      "MON_B_IN1"       },
    { A_MON_B_IN2,      "MON_B_IN2"       },
    { A_PLAY_PAUSE,     "PLAY_PAUSE"      },
    { A_PREV_TRACK,     "PREV_TRACK"      },
    { A_NEXT_TRACK,     "NEXT_TRACK"      },
    { A_VOL_UP,         "VOL_UP"          },
    { A_VOL_DOWN,       "VOL_DOWN"        },
    { A_MUTE,           "MUTE"            },
    { A_LOCK,           "LOCK"            },
    { A_SLEEP,          "SLEEP"           },
    { A_SCREENSHOT,     "SCREENSHOT"      },
    { A_REC,            "REC"             },
    { A_DND,            "DND"             },
    { A_CALC,           "CALC"            },
    { A_BRIGHT_A_UP,    "BRIGHT_A_UP"     },
    { A_BRIGHT_A_DOWN,  "BRIGHT_A_DOWN"   },
    { A_BRIGHT_B_UP,    "BRIGHT_B_UP"     },
    { A_BRIGHT_B_DOWN,  "BRIGHT_B_DOWN"   },
};
#define ACTION_NAMES_COUNT (sizeof(ACTION_NAMES) / sizeof(ACTION_NAMES[0]))

action_t action_from_name(const char *name)
{
    if (!name) return A_NONE;
    for (size_t i = 0; i < ACTION_NAMES_COUNT; i++) {
        if (strcmp(ACTION_NAMES[i].name, name) == 0) return ACTION_NAMES[i].code;
    }
    return A_NONE;
}

const char *action_name(action_t a)
{
    for (size_t i = 0; i < ACTION_NAMES_COUNT; i++) {
        if (ACTION_NAMES[i].code == a) return ACTION_NAMES[i].name;
    }
    return "NONE";
}

// ---------------------------------------------------------------------------
// In-memory config, populated by config_init() from NVS (or defaults) and
// updated in place by config_set_from_json().

static config_t s_config;

static const config_t DEFAULT_CONFIG = {
    .version = CONFIG_SCHEMA_VERSION,
    .pages = {
        [0] = { .slots = { A_KVM_PC1, A_KVM_PC2, A_MON_A_IN1, A_MON_A_IN2, A_MON_B_IN1, A_MON_B_IN2 } },
        [1] = { .slots = { A_PLAY_PAUSE, A_PREV_TRACK, A_NEXT_TRACK, A_VOL_UP, A_VOL_DOWN, A_MUTE } },
        [2] = { .slots = { A_LOCK, A_SLEEP, A_SCREENSHOT, A_REC, A_DND, A_CALC } },
    },
    .encoders = {
        [0] = { .press = A_MUTE, .twist_pos = A_VOL_UP,      .twist_neg = A_VOL_DOWN      },
        [1] = { .press = A_NONE, .twist_pos = A_BRIGHT_A_UP, .twist_neg = A_BRIGHT_A_DOWN },
        [2] = { .press = A_NONE, .twist_pos = A_BRIGHT_B_UP, .twist_neg = A_BRIGHT_B_DOWN },
    },
};

const config_t *config_get(void) { return &s_config; }

// ---------------------------------------------------------------------------
// JSON <-> struct

static cJSON *config_to_cjson(const config_t *cfg)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddNumberToObject(root, "version", cfg->version);

    cJSON *pages = cJSON_AddArrayToObject(root, "pages");
    for (int p = 0; p < KEY_ANIM_PAGE_COUNT; p++) {
        cJSON *page = cJSON_CreateObject();
        cJSON *slots = cJSON_AddArrayToObject(page, "slots");
        for (int s = 0; s < KEY_ANIM_KEY_COUNT; s++) {
            cJSON_AddItemToArray(slots, cJSON_CreateString(action_name(cfg->pages[p].slots[s])));
        }
        cJSON_AddItemToArray(pages, page);
    }

    cJSON *encs = cJSON_AddArrayToObject(root, "encoders");
    for (int e = 0; e < AKP03E_ENCODER_COUNT; e++) {
        cJSON *enc = cJSON_CreateObject();
        cJSON_AddStringToObject(enc, "press",     action_name(cfg->encoders[e].press));
        cJSON_AddStringToObject(enc, "twist_pos", action_name(cfg->encoders[e].twist_pos));
        cJSON_AddStringToObject(enc, "twist_neg", action_name(cfg->encoders[e].twist_neg));
        cJSON_AddItemToArray(encs, enc);
    }

    return root;
}

// Parse JSON into a fresh config struct. Returns ESP_OK and fills *out on
// success; otherwise leaves *out untouched.
static esp_err_t parse_json(const char *json, config_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "JSON parse failed at offset %d", (int)(cJSON_GetErrorPtr() - json));
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_ERR_INVALID_ARG;
    config_t tmp = {0};

    cJSON *ver = cJSON_GetObjectItem(root, "version");
    if (!cJSON_IsNumber(ver) || ver->valueint != CONFIG_SCHEMA_VERSION) {
        ESP_LOGW(TAG, "schema version mismatch (got %d, want %d)",
                 ver ? ver->valueint : -1, CONFIG_SCHEMA_VERSION);
        goto done;
    }
    tmp.version = ver->valueint;

    cJSON *pages = cJSON_GetObjectItem(root, "pages");
    if (!cJSON_IsArray(pages) || cJSON_GetArraySize(pages) != KEY_ANIM_PAGE_COUNT) {
        ESP_LOGW(TAG, "pages: expected array of %d", KEY_ANIM_PAGE_COUNT);
        goto done;
    }
    for (int p = 0; p < KEY_ANIM_PAGE_COUNT; p++) {
        cJSON *page = cJSON_GetArrayItem(pages, p);
        cJSON *slots = cJSON_GetObjectItem(page, "slots");
        if (!cJSON_IsArray(slots) || cJSON_GetArraySize(slots) != KEY_ANIM_KEY_COUNT) {
            ESP_LOGW(TAG, "page %d slots: expected array of %d", p, KEY_ANIM_KEY_COUNT);
            goto done;
        }
        for (int s = 0; s < KEY_ANIM_KEY_COUNT; s++) {
            cJSON *name = cJSON_GetArrayItem(slots, s);
            tmp.pages[p].slots[s] = cJSON_IsString(name)
                                    ? action_from_name(name->valuestring)
                                    : A_NONE;
        }
    }

    cJSON *encs = cJSON_GetObjectItem(root, "encoders");
    if (!cJSON_IsArray(encs) || cJSON_GetArraySize(encs) != AKP03E_ENCODER_COUNT) {
        ESP_LOGW(TAG, "encoders: expected array of %d", AKP03E_ENCODER_COUNT);
        goto done;
    }
    for (int e = 0; e < AKP03E_ENCODER_COUNT; e++) {
        cJSON *enc = cJSON_GetArrayItem(encs, e);
        cJSON *press     = cJSON_GetObjectItem(enc, "press");
        cJSON *twist_pos = cJSON_GetObjectItem(enc, "twist_pos");
        cJSON *twist_neg = cJSON_GetObjectItem(enc, "twist_neg");
        tmp.encoders[e].press     = cJSON_IsString(press)     ? action_from_name(press->valuestring)     : A_NONE;
        tmp.encoders[e].twist_pos = cJSON_IsString(twist_pos) ? action_from_name(twist_pos->valuestring) : A_NONE;
        tmp.encoders[e].twist_neg = cJSON_IsString(twist_neg) ? action_from_name(twist_neg->valuestring) : A_NONE;
    }

    *out = tmp;
    err = ESP_OK;

done:
    cJSON_Delete(root);
    return err;
}

char *config_to_json(void)
{
    cJSON *root = config_to_cjson(&s_config);
    if (!root) return NULL;
    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return str;
}

// ---------------------------------------------------------------------------
// NVS persistence

static esp_err_t nvs_save(const char *json)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, NVS_KEY_CONFIG, json);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// Reads the NVS blob into a freshly-malloced null-terminated string. Caller
// owns the result. Returns NULL if the key is missing or on any error.
static char *nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return NULL;

    size_t len = 0;
    esp_err_t err = nvs_get_str(h, NVS_KEY_CONFIG, NULL, &len);
    if (err != ESP_OK || len == 0) { nvs_close(h); return NULL; }

    char *buf = malloc(len);
    if (!buf) { nvs_close(h); return NULL; }

    err = nvs_get_str(h, NVS_KEY_CONFIG, buf, &len);
    nvs_close(h);
    if (err != ESP_OK) { free(buf); return NULL; }
    return buf;
}

esp_err_t config_set_from_json(const char *json)
{
    config_t parsed;
    esp_err_t err = parse_json(json, &parsed);
    if (err != ESP_OK) return err;

    s_config = parsed;
    err = nvs_save(json);
    if (err != ESP_OK) ESP_LOGW(TAG, "nvs save: %s", esp_err_to_name(err));
    return err;
}

esp_err_t config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs_flash_init: %s — erasing and retrying", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    char *json = nvs_load();
    if (json) {
        if (parse_json(json, &s_config) == ESP_OK) {
            ESP_LOGI(TAG, "loaded config from NVS (v%lu)", (unsigned long)s_config.version);
            free(json);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "NVS config invalid — falling back to defaults");
        free(json);
    } else {
        ESP_LOGI(TAG, "no NVS config — stamping defaults");
    }

    s_config = DEFAULT_CONFIG;
    char *serialised = config_to_json();
    if (serialised) {
        esp_err_t save = nvs_save(serialised);
        if (save != ESP_OK) ESP_LOGW(TAG, "defaults save: %s", esp_err_to_name(save));
        free(serialised);
    }
    return ESP_OK;
}
