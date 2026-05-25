#include "notify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "key_anim.h"
#include "live_key.h"
#include "lvgl.h"
#include "mqtt.h"
#include "nvs.h"

static const char *TAG = "notify";

#define NVS_NAMESPACE  "deskpad"
#define NVS_KEY        "notify_v1"

// ---------------------------------------------------------------------------
// Storage

static notification_def_t s_defs[NOTIF_MAX];
static size_t             s_def_count;
static int                s_mqtt_subs[NOTIF_MAX];   // mqtt subscription handles
static SemaphoreHandle_t  s_mtx;

// ---------------------------------------------------------------------------
// Active notification (only one shown at a time)

static struct {
    bool                 active;
    notification_def_t  *def;
    int64_t              expires_us;     // 0 = no auto-expire
    live_key_handle_t    live_handles[KEY_ANIM_KEY_COUNT];   // one per overlaid slot
} s_active;

static esp_timer_handle_t s_expire_timer;

// LVGL render — single notification renderer. Reads label + colour from
// the active def. Used by all slots an overlay claims.
static void render_overlay(lv_obj_t *canvas, void *user)
{
    (void)user;
    if (!s_active.active || !s_active.def) {
        // Painter fell through with no active notification — render black.
        lv_layer_t layer;
        lv_canvas_init_layer(canvas, &layer);
        lv_draw_rect_dsc_t bg;
        lv_draw_rect_dsc_init(&bg);
        bg.bg_color = lv_color_black();
        bg.bg_opa = LV_OPA_COVER;
        lv_area_t a = { 0, 0, 59, 59 };
        lv_draw_rect(&layer, &bg, &a);
        lv_canvas_finish_layer(canvas, &layer);
        return;
    }

    notification_def_t *d = s_active.def;
    // Parse colour_hex "#RRGGBB" — fall back to amber if invalid.
    uint8_t r = 0xE2, g = 0x4B, b = 0x4A;
    if (d->colour_hex[0] == '#' && strlen(d->colour_hex) >= 7) {
        unsigned int rr, gg, bb;
        if (sscanf(d->colour_hex + 1, "%02x%02x%02x", &rr, &gg, &bb) == 3) {
            r = (uint8_t)rr; g = (uint8_t)gg; b = (uint8_t)bb;
        }
    }

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = lv_color_make(r, g, b);
    bg.bg_opa   = LV_OPA_COVER;
    lv_area_t full = { 0, 0, 59, 59 };
    lv_draw_rect(&layer, &bg, &full);

    if (d->label[0]) {
        lv_draw_label_dsc_t lbl;
        lv_draw_label_dsc_init(&lbl);
        lbl.text  = d->label;
        lbl.color = lv_color_white();
        lbl.font  = &lv_font_montserrat_14;
        lbl.align = LV_TEXT_ALIGN_CENTER;
        int line_h = lv_font_get_line_height(lbl.font);
        int y0 = (64 - line_h) / 2;
        lv_area_t coords = { 0, y0, 63, y0 + line_h - 1 };
        lv_draw_label(&layer, &lbl, &coords);
    }

    lv_canvas_finish_layer(canvas, &layer);
}

static void on_overlay_show(void *user)
{
    live_key_handle_t h = (live_key_handle_t)user;
    live_key_invalidate(h);
}

// ---------------------------------------------------------------------------
// Activation / dismissal

static void deactivate(void);
static void on_expire(void *arg)
{
    (void)arg;
    deactivate();
}

static void activate(notification_def_t *def)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_active.active) { xSemaphoreGive(s_mtx); return; }   // simple: one-at-a-time
    s_active.active = true;
    s_active.def    = def;
    s_active.expires_us = def->duration_ms > 0
                          ? esp_timer_get_time() + (int64_t)def->duration_ms * 1000
                          : 0;
    xSemaphoreGive(s_mtx);

    ESP_LOGI(TAG, "fire '%s' (style=%d slot=%u)", def->id, def->style, def->target_slot);

    uint8_t page = key_anim_get_page();
    if (def->style == NOTIF_STYLE_TAKEOVER) {
        for (int k = 0; k < KEY_ANIM_KEY_COUNT; k++) {
            s_active.live_handles[k] = live_key_create(page, (uint8_t)k, render_overlay, NULL);
            key_anim_set_overlay(page, (uint8_t)k, on_overlay_show, s_active.live_handles[k]);
        }
    } else {
        uint8_t slot = def->target_slot < KEY_ANIM_KEY_COUNT ? def->target_slot : 0;
        s_active.live_handles[slot] = live_key_create(page, slot, render_overlay, NULL);
        key_anim_set_overlay(page, slot, on_overlay_show, s_active.live_handles[slot]);
    }

    if (def->duration_ms > 0 && s_expire_timer) {
        esp_timer_stop(s_expire_timer);
        esp_timer_start_once(s_expire_timer, (uint64_t)def->duration_ms * 1000);
    }
}

static void deactivate(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (!s_active.active) { xSemaphoreGive(s_mtx); return; }
    notification_def_t *def = s_active.def;
    s_active.active = false;
    s_active.def    = NULL;
    s_active.expires_us = 0;
    xSemaphoreGive(s_mtx);

    if (s_expire_timer) esp_timer_stop(s_expire_timer);

    uint8_t page = key_anim_get_page();
    if (def && def->style == NOTIF_STYLE_TAKEOVER) {
        for (int k = 0; k < KEY_ANIM_KEY_COUNT; k++) key_anim_clear_overlay(page, (uint8_t)k);
    } else if (def) {
        uint8_t slot = def->target_slot < KEY_ANIM_KEY_COUNT ? def->target_slot : 0;
        key_anim_clear_overlay(page, slot);
    }
    ESP_LOGI(TAG, "dismissed");
}

bool notify_overlay_active(void) { return s_active.active; }

bool notify_consume_press(uint8_t slot)
{
    (void)slot;
    if (!s_active.active || !s_active.def) return false;
    if (!s_active.def->dismiss_any_key) return false;
    deactivate();
    return true;
}

// ---------------------------------------------------------------------------
// MQTT trigger handling

static bool payload_matches(const char *payload, size_t len, const char *want)
{
    size_t wl = strlen(want);
    if (len == wl && memcmp(payload, want, wl) == 0) return true;
    // JSON: look for "state":"<want>"
    char needle[80];
    int n = snprintf(needle, sizeof(needle), "\"state\":\"%s\"", want);
    if (n > 0 && n < (int)sizeof(needle)) {
        for (size_t i = 0; i + (size_t)n <= len; i++)
            if (memcmp(payload + i, needle, (size_t)n) == 0) return true;
    }
    return false;
}

static void on_trigger(const char *topic, const char *payload, size_t payload_len, void *user)
{
    (void)topic;
    notification_def_t *d = (notification_def_t *)user;
    bool on = payload_matches(payload, payload_len, d->trigger_on_value);
    if (on) {
        activate(d);
    } else {
        // source_off dismissal
        if (s_active.active && s_active.def == d && d->dismiss_source_off) deactivate();
    }
}

static void subscribe_all(void)
{
    for (size_t i = 0; i < s_def_count; i++) {
        if (s_defs[i].trigger_topic[0])
            s_mqtt_subs[i] = mqtt_subscribe(s_defs[i].trigger_topic, on_trigger, &s_defs[i]);
        else
            s_mqtt_subs[i] = -1;
    }
}

static void unsubscribe_all(void)
{
    for (size_t i = 0; i < s_def_count; i++) {
        if (s_mqtt_subs[i] >= 0) mqtt_unsubscribe(s_mqtt_subs[i]);
        s_mqtt_subs[i] = -1;
    }
}

// ---------------------------------------------------------------------------
// NVS

static esp_err_t parse_json_into(const char *json, notification_def_t *out, size_t *count)
{
    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsArray(root)) { cJSON_Delete(root); return ESP_ERR_INVALID_ARG; }
    int n = cJSON_GetArraySize(root);
    if (n > NOTIF_MAX) n = NOTIF_MAX;
    *count = (size_t)n;
    memset(out, 0, sizeof(notification_def_t) * (size_t)n);
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_GetArrayItem(root, i);
        if (!cJSON_IsObject(o)) continue;
        cJSON *v;
        if ((v = cJSON_GetObjectItem(o, "id")) && cJSON_IsString(v))
            strncpy(out[i].id, v->valuestring, NOTIF_ID_MAX - 1);
        if ((v = cJSON_GetObjectItem(o, "priority")) && cJSON_IsNumber(v))
            out[i].priority = v->valueint;
        cJSON *trig = cJSON_GetObjectItem(o, "trigger");
        if (cJSON_IsObject(trig)) {
            if ((v = cJSON_GetObjectItem(trig, "topic")) && cJSON_IsString(v))
                strncpy(out[i].trigger_topic, v->valuestring, NOTIF_TOPIC_MAX - 1);
            if ((v = cJSON_GetObjectItem(trig, "on_value")) && cJSON_IsString(v))
                strncpy(out[i].trigger_on_value, v->valuestring, NOTIF_VALUE_MAX - 1);
        }
        cJSON *disp = cJSON_GetObjectItem(o, "display");
        if (cJSON_IsObject(disp)) {
            if ((v = cJSON_GetObjectItem(disp, "style")) && cJSON_IsString(v))
                out[i].style = (strcmp(v->valuestring, "overlay") == 0)
                                ? NOTIF_STYLE_OVERLAY : NOTIF_STYLE_TAKEOVER;
            if ((v = cJSON_GetObjectItem(disp, "target_slot")) && cJSON_IsNumber(v))
                out[i].target_slot = (uint8_t)v->valueint;
            if ((v = cJSON_GetObjectItem(disp, "label")) && cJSON_IsString(v))
                strncpy(out[i].label, v->valuestring, NOTIF_LABEL_MAX - 1);
            if ((v = cJSON_GetObjectItem(disp, "colour")) && cJSON_IsString(v))
                strncpy(out[i].colour_hex, v->valuestring, NOTIF_COLOUR_MAX - 1);
            if ((v = cJSON_GetObjectItem(disp, "duration_ms")) && cJSON_IsNumber(v))
                out[i].duration_ms = v->valueint;
            if ((v = cJSON_GetObjectItem(disp, "dismiss_any_key")) && cJSON_IsBool(v))
                out[i].dismiss_any_key = cJSON_IsTrue(v);
            if ((v = cJSON_GetObjectItem(disp, "dismiss_source_off")) && cJSON_IsBool(v))
                out[i].dismiss_source_off = cJSON_IsTrue(v);
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static char *serialise_defs(const notification_def_t *defs, size_t n)
{
    cJSON *root = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", defs[i].id);
        cJSON_AddNumberToObject(o, "priority", defs[i].priority);
        cJSON *trig = cJSON_AddObjectToObject(o, "trigger");
        cJSON_AddStringToObject(trig, "topic",    defs[i].trigger_topic);
        cJSON_AddStringToObject(trig, "on_value", defs[i].trigger_on_value);
        cJSON *disp = cJSON_AddObjectToObject(o, "display");
        cJSON_AddStringToObject(disp, "style",
                                 defs[i].style == NOTIF_STYLE_OVERLAY ? "overlay" : "takeover");
        cJSON_AddNumberToObject(disp, "target_slot",      defs[i].target_slot);
        cJSON_AddStringToObject(disp, "label",            defs[i].label);
        cJSON_AddStringToObject(disp, "colour",           defs[i].colour_hex);
        cJSON_AddNumberToObject(disp, "duration_ms",      defs[i].duration_ms);
        cJSON_AddBoolToObject  (disp, "dismiss_any_key",  defs[i].dismiss_any_key);
        cJSON_AddBoolToObject  (disp, "dismiss_source_off", defs[i].dismiss_source_off);
        cJSON_AddItemToArray(root, o);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static esp_err_t persist_defs(void)
{
    char *json = serialise_defs(s_defs, s_def_count);
    if (!json) return ESP_ERR_NO_MEM;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY, json);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    free(json);
    return err;
}

// ---------------------------------------------------------------------------
// Public

size_t notify_get_all(notification_def_t *out, size_t max)
{
    if (!out) return 0;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    size_t n = s_def_count < max ? s_def_count : max;
    memcpy(out, s_defs, n * sizeof(notification_def_t));
    xSemaphoreGive(s_mtx);
    return n;
}

esp_err_t notify_set_all(const notification_def_t *defs, size_t n)
{
    if (!defs || n > NOTIF_MAX) return ESP_ERR_INVALID_ARG;
    deactivate();
    unsubscribe_all();
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    memcpy(s_defs, defs, n * sizeof(notification_def_t));
    s_def_count = n;
    xSemaphoreGive(s_mtx);
    subscribe_all();
    return persist_defs();
}

esp_err_t notify_init(void)
{
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    if (!s_mtx) return ESP_ERR_NO_MEM;
    for (size_t i = 0; i < NOTIF_MAX; i++) s_mqtt_subs[i] = -1;

    const esp_timer_create_args_t args = {
        .callback = on_expire,
        .name = "notify_expire",
    };
    esp_timer_create(&args, &s_expire_timer);

    // Load from NVS.
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = 0;
        if (nvs_get_str(h, NVS_KEY, NULL, &len) == ESP_OK && len > 0) {
            char *buf = malloc(len);
            if (buf && nvs_get_str(h, NVS_KEY, buf, &len) == ESP_OK) {
                parse_json_into(buf, s_defs, &s_def_count);
            }
            free(buf);
        }
        nvs_close(h);
    }

    subscribe_all();
    ESP_LOGI(TAG, "init: %u notifications configured", (unsigned)s_def_count);
    return ESP_OK;
}
