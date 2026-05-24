#include "live_key.h"
#include "akp03e.h"
#include "jpeg_enc.h"
#include "key_anim.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "live_key";

#define W 60
#define H 60
#define MAX_LIVE_KEYS 8

struct live_key_s {
    uint8_t page;
    uint8_t key;
    live_key_render_fn_t render;
    void *user;
};

static struct live_key_s s_keys[MAX_LIVE_KEYS];
static int s_key_count;
static QueueHandle_t s_q;

// Reusable scratch buffers — single worker task, shared safely.
// LVGL canvas buffer must stay valid while the canvas exists, so it's static.
static uint8_t s_canvas_buf[W * H * 3];

// LVGL v9 requires a default display for widget allocation. We don't have a
// real one (the AKP isn't framebuffer-mapped), so spin up a 1x1 stub whose
// flush callback is a no-op. Its active screen becomes the parent for every
// canvas we create.
static lv_display_t *s_stub_disp;
static uint8_t s_stub_disp_buf[64];

static void stub_flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *buf)
{
    (void)area; (void)buf;
    lv_display_flush_ready(d);
}

static void rotate_cw_to_jpeg_input(const uint8_t *src)
{
    uint8_t *dst = jpeg_enc_input_buf();
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int s = (y * W + x) * 3;
            int d = (x * H + (H - 1 - y)) * 3;
            dst[d + 0] = src[s + 0];
            dst[d + 1] = src[s + 1];
            dst[d + 2] = src[s + 2];
        }
    }
}

static void render_one(struct live_key_s *lk)
{
    // 1. Create a canvas as a child of the stub display's screen, wrapping
    //    our static RGB888 buffer.
    lv_obj_t *parent = lv_display_get_screen_active(s_stub_disp);
    lv_obj_t *canvas = lv_canvas_create(parent);
    if (!canvas) {
        ESP_LOGW(TAG, "lv_canvas_create failed");
        return;
    }
    lv_canvas_set_buffer(canvas, s_canvas_buf, W, H, LV_COLOR_FORMAT_RGB888);
    lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_COVER);

    // 2. Let the user draw on it.
    lk->render(canvas, lk->user);

    lv_obj_delete(canvas);  // detaches buffer; doesn't free it

    // 3. Rotate 90° CW so the device's own +90° rotation lands it upright.
    rotate_cw_to_jpeg_input(s_canvas_buf);

    // 4. Hardware-JPEG-encode.
    size_t jpeg_size = 0;
    const uint8_t *jpeg = jpeg_enc_encode(&jpeg_size);
    if (!jpeg) {
        ESP_LOGW(TAG, "jpeg encode failed");
        return;
    }

    // 5. Push to the device (only meaningful when on the live key's page).
    if (key_anim_get_page() != lk->page) return;
    esp_err_t err = akp03e_set_key_jpeg(lk->key, jpeg, jpeg_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_key_jpeg[p%u k%u] -> %s",
                 lk->page, lk->key, esp_err_to_name(err));
    }
}

static void worker(void *arg)
{
    (void)arg;
    struct live_key_s *lk = NULL;
    for (;;) {
        if (xQueueReceive(s_q, &lk, portMAX_DELAY) == pdTRUE && lk) {
            render_one(lk);
        }
    }
}

// Called by key_anim when the user switches to this slot's page.
static void on_page_shown(void *user)
{
    live_key_invalidate((live_key_handle_t)user);
}

esp_err_t live_key_init(void)
{
    if (s_q) return ESP_OK;
    esp_err_t err = jpeg_enc_init();
    if (err != ESP_OK) return err;

    // 1x1 stub display — we never actually draw through it, it just provides
    // a default screen for orphan canvas widgets to parent onto.
    s_stub_disp = lv_display_create(1, 1);
    if (!s_stub_disp) return ESP_ERR_NO_MEM;
    lv_display_set_buffers(s_stub_disp, s_stub_disp_buf, NULL,
                           sizeof(s_stub_disp_buf),
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(s_stub_disp, stub_flush_cb);

    s_q = xQueueCreate(8, sizeof(struct live_key_s *));
    if (!s_q) return ESP_ERR_NO_MEM;

    // Slightly bigger stack — LVGL canvas creation + label rendering uses
    // ~2-3 KB of stack on top of normal call depth.
    BaseType_t ok = xTaskCreate(worker, "live_key", 8192, NULL, 4, NULL);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

live_key_handle_t live_key_create(uint8_t page, uint8_t key,
                                  live_key_render_fn_t render, void *user)
{
    if (s_key_count >= MAX_LIVE_KEYS) return NULL;
    struct live_key_s *lk = &s_keys[s_key_count++];
    lk->page   = page;
    lk->key    = key;
    lk->render = render;
    lk->user   = user;
    key_anim_bind_live(page, key, on_page_shown, lk);
    return lk;
}

void live_key_invalidate(live_key_handle_t h)
{
    if (!s_q || !h) return;
    xQueueSend(s_q, &h, 0);  // drop on full; we'll catch up on next invalidate
}
