#include "clock_render.h"

#include "config.h"
#include "key_anim.h"
#include "live_key.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include <stdio.h>

static const char *TAG = "clock_render";

#define MAX_CLOCK_KEYS (KEY_ANIM_PAGE_COUNT * KEY_ANIM_KEY_COUNT)
#define UPDATE_MS 1000

typedef struct {
    uint8_t           page;
    uint8_t           key;
    live_key_handle_t handle;
} clock_slot_t;

static clock_slot_t s_slots[MAX_CLOCK_KEYS];
static int          s_slot_count;

static void render(lv_obj_t *canvas, void *user)
{
    (void)user;
    int64_t ms = esp_timer_get_time() / 1000;
    int mm = (int)((ms / 60000) % 100);   // wraps at 99 minutes — fine for a stub clock
    int ss = (int)((ms / 1000)  % 60);
    char text[8];
    snprintf(text, sizeof(text), "%02d:%02d", mm, ss);

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.text = text;
    dsc.color = lv_color_white();
    dsc.font = &lv_font_montserrat_18;
    dsc.align = LV_TEXT_ALIGN_CENTER;

    int line_h = lv_font_get_line_height(dsc.font);
    int y0 = (64 - line_h) / 2;
    lv_area_t coords = { 0, y0, 63, y0 + line_h - 1 };
    lv_draw_label(&layer, &dsc, &coords);

    lv_canvas_finish_layer(canvas, &layer);
}

static void tick_cb(void *arg)
{
    (void)arg;
    for (int i = 0; i < s_slot_count; i++) live_key_invalidate(s_slots[i].handle);
}

esp_err_t clock_render_init(void)
{
    const config_t *cfg = config_get();
    if (!cfg) return ESP_ERR_INVALID_STATE;

    for (int p = 0; p < KEY_ANIM_PAGE_COUNT; p++) {
        for (int k = 0; k < KEY_ANIM_KEY_COUNT; k++) {
            if (cfg->pages[p].slots[k].renderer != REND_CLOCK) continue;
            if (s_slot_count >= MAX_CLOCK_KEYS) {
                ESP_LOGW(TAG, "clock slot table full (%d)", MAX_CLOCK_KEYS);
                break;
            }
            s_slots[s_slot_count++] = (clock_slot_t){
                .page = (uint8_t)p, .key = (uint8_t)k, .handle = NULL,
            };
        }
    }

    for (int i = 0; i < s_slot_count; i++) {
        s_slots[i].handle = live_key_create(s_slots[i].page, s_slots[i].key,
                                             render, NULL);
        if (!s_slots[i].handle) {
            ESP_LOGE(TAG, "live_key_create p%u k%u failed",
                     s_slots[i].page, s_slots[i].key);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "bound clock to p%u k%u", s_slots[i].page, s_slots[i].key);
    }

    if (s_slot_count == 0) {
        ESP_LOGI(TAG, "no slots configured as clock — skipping timer");
        return ESP_OK;
    }

    const esp_timer_create_args_t args = {
        .callback = tick_cb,
        .name = "clock_render",
    };
    esp_timer_handle_t timer;
    esp_err_t err = esp_timer_create(&args, &timer);
    if (err != ESP_OK) return err;
    return esp_timer_start_periodic(timer, UPDATE_MS * 1000);  // µs
}
