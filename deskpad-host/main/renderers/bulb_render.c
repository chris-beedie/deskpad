#include "bulb_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "esp_log.h"
#include "key_anim.h"
#include "live_key.h"
#include "lvgl.h"
#include "mqtt.h"

static const char *TAG = "bulb_render";

#define MAX_BULB_KEYS (KEY_ANIM_PAGE_COUNT * KEY_ANIM_KEY_COUNT)
#define DEFAULT_ON_VALUE "on"

typedef enum { STATE_UNKNOWN = 0, STATE_OFF, STATE_ON } bulb_state_t;

typedef struct {
    uint8_t           page;
    uint8_t           key;
    char             *label;          // borrowed (config-stable pointer)
    char             *state_topic;    // owned; derived if config doesn't provide one
    char             *on_value;       // borrowed; defaulted to DEFAULT_ON_VALUE if null
    bulb_state_t      state;
    live_key_handle_t handle;
    int               mqtt_sub;
} bulb_slot_t;

static bulb_slot_t s_slots[MAX_BULB_KEYS];
static int         s_slot_count;

// Compose `homeassistant/<domain>/<id>/state` from "domain.id".
static char *derive_state_topic(const char *entity)
{
    if (!entity || !*entity) return NULL;
    const char *dot = strchr(entity, '.');
    if (!dot) return NULL;
    size_t domain_len = (size_t)(dot - entity);
    const char *id = dot + 1;
    char *topic = NULL;
    asprintf(&topic, "homeassistant/%.*s/%s/state", (int)domain_len, entity, id);
    return topic;
}

// ---------------------------------------------------------------------------
// Drawing

static void render(lv_obj_t *canvas, void *user)
{
    bulb_slot_t *b = (bulb_slot_t *)user;
    bool lit = (b->state == STATE_ON);
    bool known = (b->state != STATE_UNKNOWN);

    lv_color_t bulb_fill = lit  ? lv_color_make(255, 196, 60)
                                : lv_color_make(50, 50, 56);
    lv_color_t bulb_outline = known ? lv_color_make(180, 180, 185)
                                    : lv_color_make(90, 90, 95);
    lv_color_t label_colour = lit  ? lv_color_white()
                                    : lv_color_make(150, 150, 155);

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    // Bulb body — circle centred at (30, 26), radius 14.
    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);
    arc_dsc.color = bulb_outline;
    arc_dsc.width = 2;
    arc_dsc.center.x = 30;
    arc_dsc.center.y = 26;
    arc_dsc.radius = 14;
    arc_dsc.start_angle = 0;
    arc_dsc.end_angle   = 360;
    lv_draw_arc(&layer, &arc_dsc);

    if (lit) {
        // Fill the bulb with a softer disc.
        lv_draw_arc_dsc_t fill = arc_dsc;
        fill.color = bulb_fill;
        fill.width = 12;
        fill.radius = 8;
        lv_draw_arc(&layer, &fill);
    }

    // Bulb base (small rect under the circle).
    lv_draw_rect_dsc_t base;
    lv_draw_rect_dsc_init(&base);
    base.bg_color = bulb_outline;
    base.bg_opa   = LV_OPA_COVER;
    base.radius   = 1;
    lv_area_t base_area = { 25, 40, 34, 44 };
    lv_draw_rect(&layer, &base, &base_area);

    // Label centred along the bottom.
    if (b->label && *b->label) {
        lv_draw_label_dsc_t dsc;
        lv_draw_label_dsc_init(&dsc);
        dsc.text  = b->label;
        dsc.color = label_colour;
        dsc.font  = &lv_font_montserrat_14;
        dsc.align = LV_TEXT_ALIGN_CENTER;
        int line_h = lv_font_get_line_height(dsc.font);
        lv_area_t coords = { 0, 64 - line_h - 1, 63, 63 };
        lv_draw_label(&layer, &dsc, &coords);
    }

    lv_canvas_finish_layer(canvas, &layer);
}

// ---------------------------------------------------------------------------
// MQTT message routing

static bool payload_matches(const char *payload, size_t len, const char *want)
{
    size_t wl = strlen(want);
    if (len == wl && memcmp(payload, want, wl) == 0) return true;
    // Also accept JSON `{"state":"on"}` style — find `"state":"<want>"`.
    // Quick-and-dirty substring search.
    char needle[80];
    int n = snprintf(needle, sizeof(needle), "\"state\":\"%s\"", want);
    if (n > 0 && n < (int)sizeof(needle)) {
        for (size_t i = 0; i + (size_t)n <= len; i++) {
            if (memcmp(payload + i, needle, (size_t)n) == 0) return true;
        }
    }
    return false;
}

static void on_message(const char *topic, const char *payload, size_t payload_len, void *user)
{
    (void)topic;
    bulb_slot_t *b = (bulb_slot_t *)user;
    const char *on_value = b->on_value && *b->on_value ? b->on_value : DEFAULT_ON_VALUE;
    bulb_state_t prev = b->state;
    b->state = payload_matches(payload, payload_len, on_value) ? STATE_ON : STATE_OFF;
    if (b->state != prev) live_key_invalidate(b->handle);
}

// ---------------------------------------------------------------------------
// Init

esp_err_t bulb_render_init(void)
{
    const config_t *cfg = config_get();
    if (!cfg) return ESP_ERR_INVALID_STATE;

    for (int p = 0; p < KEY_ANIM_PAGE_COUNT; p++) {
        for (int k = 0; k < KEY_ANIM_KEY_COUNT; k++) {
            const slot_config_t *s = &cfg->pages[p].slots[k];
            if (s->renderer != REND_BULB) continue;
            if (s_slot_count >= MAX_BULB_KEYS) { ESP_LOGW(TAG, "table full"); break; }

            char *topic = NULL;
            if (s->display.state_topic && *s->display.state_topic) {
                topic = strdup(s->display.state_topic);
            } else if (s->entity && *s->entity) {
                topic = derive_state_topic(s->entity);
            }

            bulb_slot_t *b = &s_slots[s_slot_count++];
            b->page        = (uint8_t)p;
            b->key         = (uint8_t)k;
            b->label       = s->display.label;
            b->state_topic = topic;
            b->on_value    = s->display.on_value;
            b->state       = STATE_UNKNOWN;
            b->handle      = NULL;
            b->mqtt_sub    = -1;
        }
    }

    for (int i = 0; i < s_slot_count; i++) {
        s_slots[i].handle = live_key_create(s_slots[i].page, s_slots[i].key,
                                             render, &s_slots[i]);
        if (!s_slots[i].handle) {
            ESP_LOGE(TAG, "live_key_create p%u k%u failed", s_slots[i].page, s_slots[i].key);
            return ESP_FAIL;
        }
        if (s_slots[i].state_topic) {
            s_slots[i].mqtt_sub = mqtt_subscribe(s_slots[i].state_topic,
                                                  on_message, &s_slots[i]);
            ESP_LOGI(TAG, "bulb p%u k%u <- %s",
                     s_slots[i].page, s_slots[i].key, s_slots[i].state_topic);
        } else {
            ESP_LOGW(TAG, "bulb p%u k%u: no state topic (need slot.entity or display.state_topic)",
                     s_slots[i].page, s_slots[i].key);
        }
    }
    return ESP_OK;
}
