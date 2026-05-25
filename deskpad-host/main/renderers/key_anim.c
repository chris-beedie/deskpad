#include "key_anim.h"

#include "akp03e.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "key_anim";

#define DECL_IMG(name) \
    extern const uint8_t name##_start[] asm("_binary_" #name "_start"); \
    extern const uint8_t name##_end[]   asm("_binary_" #name "_end")

// 3 pages x 6 keys x (default, pressed) = 36 embeds.
#define DECL_PAGE(p) \
    DECL_IMG(page##p##_key0_jpg); DECL_IMG(page##p##_key1_jpg); \
    DECL_IMG(page##p##_key2_jpg); DECL_IMG(page##p##_key3_jpg); \
    DECL_IMG(page##p##_key4_jpg); DECL_IMG(page##p##_key5_jpg); \
    DECL_IMG(page##p##_key0_pressed_jpg); DECL_IMG(page##p##_key1_pressed_jpg); \
    DECL_IMG(page##p##_key2_pressed_jpg); DECL_IMG(page##p##_key3_pressed_jpg); \
    DECL_IMG(page##p##_key4_pressed_jpg); DECL_IMG(page##p##_key5_pressed_jpg)
DECL_PAGE(0);
DECL_PAGE(1);
DECL_PAGE(2);

typedef struct { const uint8_t *s, *e; } blob_t;

#define BLOB(name) { name##_start, name##_end }
#define PAGE_DEFAULT(p) { \
    BLOB(page##p##_key0_jpg), BLOB(page##p##_key1_jpg), BLOB(page##p##_key2_jpg), \
    BLOB(page##p##_key3_jpg), BLOB(page##p##_key4_jpg), BLOB(page##p##_key5_jpg)  \
}
#define PAGE_PRESSED(p) { \
    BLOB(page##p##_key0_pressed_jpg), BLOB(page##p##_key1_pressed_jpg), \
    BLOB(page##p##_key2_pressed_jpg), BLOB(page##p##_key3_pressed_jpg), \
    BLOB(page##p##_key4_pressed_jpg), BLOB(page##p##_key5_pressed_jpg)  \
}

static const blob_t DEFAULTS[KEY_ANIM_PAGE_COUNT][KEY_ANIM_KEY_COUNT] = {
    PAGE_DEFAULT(0), PAGE_DEFAULT(1), PAGE_DEFAULT(2),
};
static const blob_t PRESSED[KEY_ANIM_PAGE_COUNT][KEY_ANIM_KEY_COUNT] = {
    PAGE_PRESSED(0), PAGE_PRESSED(1), PAGE_PRESSED(2),
};

typedef struct {
    uint8_t key;
    uint8_t pressed;
} req_t;

static QueueHandle_t s_q;
static volatile uint8_t s_page;

typedef struct {
    key_anim_on_show_cb_t on_show;
    void *user;
} live_binding_t;
static live_binding_t s_live[KEY_ANIM_PAGE_COUNT][KEY_ANIM_KEY_COUNT];
static live_binding_t s_overlay[KEY_ANIM_PAGE_COUNT][KEY_ANIM_KEY_COUNT];

// True if either overlay OR live binding wants to own this slot's visual.
static bool slot_owned(uint8_t page, uint8_t key)
{
    return s_overlay[page][key].on_show != NULL
        || s_live[page][key].on_show    != NULL;
}

static void worker(void *arg)
{
    (void)arg;
    req_t r;
    for (;;) {
        if (xQueueReceive(s_q, &r, portMAX_DELAY) != pdTRUE) continue;
        if (r.key >= KEY_ANIM_KEY_COUNT) continue;
        uint8_t page = s_page;
        if (page >= KEY_ANIM_PAGE_COUNT) page = 0;
        const blob_t *b = r.pressed ? &PRESSED[page][r.key] : &DEFAULTS[page][r.key];
        esp_err_t err = akp03e_set_key_jpeg(r.key, b->s, (size_t)(b->e - b->s));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "set_key_jpeg[p%u k%u %s] -> %s",
                     page, r.key, r.pressed ? "pressed" : "default", esp_err_to_name(err));
        }
    }
}

esp_err_t key_anim_init(void)
{
    if (s_q) return ESP_OK;
    s_q = xQueueCreate(16, sizeof(req_t));
    if (!s_q) return ESP_ERR_NO_MEM;
    s_page = 0;
    BaseType_t ok = xTaskCreate(worker, "key_anim", 4096, NULL, 4, NULL);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

static void queue_one(uint8_t key, uint8_t pressed)
{
    if (!s_q || key >= KEY_ANIM_KEY_COUNT) return;
    req_t r = { .key = key, .pressed = pressed };
    xQueueSend(s_q, &r, 0);  // drop on full; latest-wins is fine
}

void key_anim_show_default(uint8_t key)
{
    if (key < KEY_ANIM_KEY_COUNT && slot_owned(s_page, key)) return;
    queue_one(key, 0);
}
void key_anim_show_pressed(uint8_t key)
{
    if (key < KEY_ANIM_KEY_COUNT && slot_owned(s_page, key)) return;
    queue_one(key, 1);
}

void key_anim_set_page(uint8_t page)
{
    if (page >= KEY_ANIM_PAGE_COUNT) page = KEY_ANIM_PAGE_COUNT - 1;
    s_page = page;
    ESP_LOGI(TAG, "page -> %u", page);
    for (uint8_t k = 0; k < KEY_ANIM_KEY_COUNT; k++) {
        if (s_overlay[page][k].on_show) {
            s_overlay[page][k].on_show(s_overlay[page][k].user);
        } else if (s_live[page][k].on_show) {
            s_live[page][k].on_show(s_live[page][k].user);
        } else {
            queue_one(k, 0);
        }
    }
}

void key_anim_bind_live(uint8_t page, uint8_t key,
                        key_anim_on_show_cb_t on_show, void *user)
{
    if (page >= KEY_ANIM_PAGE_COUNT || key >= KEY_ANIM_KEY_COUNT) return;
    s_live[page][key].on_show = on_show;
    s_live[page][key].user    = user;
}

bool key_anim_is_live(uint8_t page, uint8_t key)
{
    return page < KEY_ANIM_PAGE_COUNT && key < KEY_ANIM_KEY_COUNT
           && s_live[page][key].on_show != NULL;
}

void key_anim_set_overlay(uint8_t page, uint8_t key,
                          key_anim_on_show_cb_t on_show, void *user)
{
    if (page >= KEY_ANIM_PAGE_COUNT || key >= KEY_ANIM_KEY_COUNT) return;
    s_overlay[page][key].on_show = on_show;
    s_overlay[page][key].user    = user;
    if (page == s_page && on_show) on_show(user);   // immediate paint
}

void key_anim_clear_overlay(uint8_t page, uint8_t key)
{
    if (page >= KEY_ANIM_PAGE_COUNT || key >= KEY_ANIM_KEY_COUNT) return;
    s_overlay[page][key].on_show = NULL;
    s_overlay[page][key].user    = NULL;
    if (page == s_page) {
        // Repaint with the underlying base (live or static).
        if (s_live[page][key].on_show) {
            s_live[page][key].on_show(s_live[page][key].user);
        } else {
            queue_one(key, 0);
        }
    }
}

uint8_t key_anim_get_page(void) { return s_page; }
