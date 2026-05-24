#include "clock_key.h"
#include "live_key.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include <stdio.h>

static const char *TAG = "clock_key";

#define CLOCK_PAGE 2
#define CLOCK_KEY  5
#define UPDATE_MS  1000          // re-render every second; "MM:SS" wants 1s granularity

static live_key_handle_t s_handle;

static void render(lv_obj_t *canvas, void *user)
{
    (void)user;
    int64_t ms = esp_timer_get_time() / 1000;
    int mm = (int)((ms / 60000) % 100);   // wraps at 99 minutes — fine for a stub
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

    // Centre vertically: font line height ~22 px, canvas 60 -> offset ~19.
    int line_h = lv_font_get_line_height(dsc.font);
    int y0 = (60 - line_h) / 2;
    lv_area_t coords = { 0, y0, 59, y0 + line_h - 1 };
    lv_draw_label(&layer, &dsc, &coords);

    lv_canvas_finish_layer(canvas, &layer);
}

static void tick_cb(void *arg)
{
    (void)arg;
    live_key_invalidate(s_handle);
}

esp_err_t clock_key_init(void)
{
    s_handle = live_key_create(CLOCK_PAGE, CLOCK_KEY, render, NULL);
    if (!s_handle) {
        ESP_LOGE(TAG, "live_key_create failed");
        return ESP_FAIL;
    }
    const esp_timer_create_args_t args = {
        .callback = tick_cb,
        .name = "clock_key",
    };
    esp_timer_handle_t timer;
    esp_err_t err = esp_timer_create(&args, &timer);
    if (err != ESP_OK) return err;
    return esp_timer_start_periodic(timer, UPDATE_MS * 1000);  // µs
}
