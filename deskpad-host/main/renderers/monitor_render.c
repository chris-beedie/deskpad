#include "monitor_render.h"

#include "config.h"
#include "host_state.h"
#include "live_key.h"
#include "key_anim.h"
#include "esp_log.h"
#include "lvgl.h"

#include <string.h>

static const char *TAG = "monitor_render";

#define MAX_MONITOR_KEYS (KEY_ANIM_PAGE_COUNT * KEY_ANIM_KEY_COUNT)

typedef struct {
    uint8_t           page;
    uint8_t           key;
    host_t            lit_when;
    const char       *label;     // borrowed from config (pointer-stable per config_get())
    live_key_handle_t handle;
} mon_slot_t;

static mon_slot_t s_slots[MAX_MONITOR_KEYS];
static int        s_slot_count;

// Geometry — 60x60 canvas, monitor icon centred. Numbers tuned by eye.
#define ICON_X      4
#define ICON_Y      6
#define ICON_W     52
#define ICON_H     36
#define STAND_X    22
#define STAND_Y    44
#define STAND_W    16
#define STAND_H     5
#define BASE_X     12
#define BASE_Y    50
#define BASE_W     36
#define BASE_H      3

static void fill_rect(lv_layer_t *layer, int x, int y, int w, int h, lv_color_t colour)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = colour;
    dsc.bg_opa   = LV_OPA_COVER;
    dsc.border_width = 0;
    dsc.radius   = 2;
    lv_area_t area = { x, y, x + w - 1, y + h - 1 };
    lv_draw_rect(layer, &dsc, &area);
}

static void draw_centered_label(lv_layer_t *layer, const char *text,
                                int x, int y, int w, int h,
                                lv_color_t colour, const lv_font_t *font)
{
    if (!text || !*text) return;
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.text = text;
    dsc.color = colour;
    dsc.font  = font;
    dsc.align = LV_TEXT_ALIGN_CENTER;
    int line_h = lv_font_get_line_height(font);
    int y0 = y + (h - line_h) / 2;
    lv_area_t area = { x, y0, x + w - 1, y0 + line_h - 1 };
    lv_draw_label(layer, &dsc, &area);
}

static void render(lv_obj_t *canvas, void *user)
{
    mon_slot_t *m = (mon_slot_t *)user;
    bool lit = (m->lit_when != 0) && (host_state_get_active() == m->lit_when);

    lv_color_t screen = lit ? lv_color_make(46, 160, 220)   // bright cyan-blue
                            : lv_color_make(40, 40, 48);    // dim slate
    lv_color_t chrome = lv_color_make(160, 160, 165);       // monitor bezel / stand
    lv_color_t label_colour = lit ? lv_color_white() : lv_color_make(150, 150, 155);

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    // Bezel (slightly larger than screen)
    fill_rect(&layer, ICON_X - 2, ICON_Y - 2, ICON_W + 4, ICON_H + 4, chrome);
    // Screen
    fill_rect(&layer, ICON_X, ICON_Y, ICON_W, ICON_H, screen);
    // Stand neck
    fill_rect(&layer, STAND_X, STAND_Y, STAND_W, STAND_H, chrome);
    // Base
    fill_rect(&layer, BASE_X, BASE_Y, BASE_W, BASE_H, chrome);

    draw_centered_label(&layer, m->label,
                        ICON_X, ICON_Y, ICON_W, ICON_H,
                        label_colour, &lv_font_montserrat_14);

    lv_canvas_finish_layer(canvas, &layer);
}

static void on_active_change(host_t new_active, void *user)
{
    (void)new_active; (void)user;
    for (int i = 0; i < s_slot_count; i++) {
        live_key_invalidate(s_slots[i].handle);
    }
}

esp_err_t monitor_render_init(void)
{
    const config_t *cfg = config_get();
    if (!cfg) return ESP_ERR_INVALID_STATE;

    for (int p = 0; p < KEY_ANIM_PAGE_COUNT; p++) {
        for (int k = 0; k < KEY_ANIM_KEY_COUNT; k++) {
            const slot_config_t *s = &cfg->pages[p].slots[k];
            if (s->renderer != REND_MONITOR) continue;
            if (s_slot_count >= MAX_MONITOR_KEYS) {
                ESP_LOGW(TAG, "monitor slot table full (%d)", MAX_MONITOR_KEYS);
                break;
            }
            s_slots[s_slot_count++] = (mon_slot_t){
                .page = (uint8_t)p, .key = (uint8_t)k,
                .lit_when = s->display.lit_when,
                .label    = s->display.label,
                .handle   = NULL,
            };
        }
    }

    for (int i = 0; i < s_slot_count; i++) {
        s_slots[i].handle = live_key_create(s_slots[i].page, s_slots[i].key,
                                             render, &s_slots[i]);
        if (!s_slots[i].handle) {
            ESP_LOGE(TAG, "live_key_create p%u k%u failed",
                     s_slots[i].page, s_slots[i].key);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "bound p%u k%u lit_when=%s label=\"%s\"",
                 s_slots[i].page, s_slots[i].key,
                 s_slots[i].lit_when ? host_name(s_slots[i].lit_when) : "(none)",
                 s_slots[i].label ? s_slots[i].label : "");
    }

    host_state_subscribe(on_active_change, NULL);
    return ESP_OK;
}
