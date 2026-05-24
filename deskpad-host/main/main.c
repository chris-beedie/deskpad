#include "akp03e.h"
#include "actions.h"
#include "clock_key.h"
#include "config.h"
#include "ddc.h"
#include "http_server.h"
#include "key_anim.h"
#include "live_key.h"
#include "monitor_render.h"
#include "network.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "usb/usb_host.h"

// LVGL needs a monotonic ms tick source — back it with esp_timer.
static uint32_t lv_tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static const char *TAG = "app";

#define LCD_KEY_COUNT 6

static void usb_host_lib_task(void *arg)
{
    // The AKP03E rev 2 declares both its interrupt IN (0x82) and interrupt OUT
    // (0x03) endpoints with wMaxPacketSize = 1024. The default FIFO bias on
    // ESP32-P4 caps periodic-OUT MPS at 512 bytes (BIAS_BALANCED) and likewise
    // caps IN MPS when periodic-OUT bias is selected. Carve a custom FIFO that
    // gives both directions enough room for 1024-byte packets.
    //
    // ESP32-P4 HS FIFO depth = 1024 lines (1 line = 4 bytes).
    //   in_mps             = (rx  - 2) * 4  -> rx  >= 258 lines for 1024B IN
    //   periodic_out_mps   = ptx       * 4  -> ptx >= 256 lines for 1024B OUT
    //   non_periodic_out   = nptx      * 4  -> any leftover; control/bulk only
    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        .fifo_settings_custom = {
            // ESP32-P4 HS reports a FIFO depth via GHWCFG3.DfifoDepth that we
            // can't know at build time; the validator rejected total=1024 lines
            // so the chip is smaller. Provide the minimum needed for 1024-byte
            // MPS in both directions (515 lines) plus a hair of slack — well
            // under any plausible silicon limit.
            .rx_fifo_lines   = 260,   // -> IN MPS = (260-2)*4 = 1032 B
            .ptx_fifo_lines  = 260,   // -> periodic-OUT MPS = 1040 B
            .nptx_fifo_lines = 64,    // small; only used for control transfers
        },
    };
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));
    xTaskNotifyGive((TaskHandle_t)arg);

    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGI(TAG, "usb host: no clients");
        }
        if (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "usb host: all devices freed");
        }
    }
}

static void on_akp_event(const akp03e_event_t *ev, void *user)
{
    (void)user;
    switch (ev->type) {
    case AKP03E_EVT_BUTTON:
        ESP_LOGI(TAG, "button %u %s", ev->index, ev->pressed ? "DOWN" : "UP");
        if (ev->index < LCD_KEY_COUNT) {
            if (ev->pressed) key_anim_show_pressed(ev->index);
            else             key_anim_show_default(ev->index);
        }
        actions_handle(ev);
        break;
    case AKP03E_EVT_ENCODER_PRESS:
        ESP_LOGI(TAG, "encoder %u %s", ev->index, ev->pressed ? "DOWN" : "UP");
        actions_handle(ev);
        break;
    case AKP03E_EVT_ENCODER_TWIST:
        ESP_LOGI(TAG, "encoder %u twist %+d", ev->index, ev->twist);
        actions_handle(ev);
        break;
    case AKP03E_EVT_CONNECTED:
        ESP_LOGI(TAG, "akp03e connected, setting brightness, pushing images");
        akp03e_set_brightness(70);
        akp03e_clear_all_keys();
        key_anim_set_page(0);
        break;
    case AKP03E_EVT_DISCONNECTED:
        ESP_LOGW(TAG, "akp03e disconnected");
        break;
    }
}

// Tell the bootloader the current image works. Required when
// CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y — otherwise the bootloader
// rolls back to the previous slot on the next reset.
static void ota_mark_self_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) return;
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "OTA image pending verify — marking valid");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "boot");
    ota_mark_self_valid();

    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    xTaskCreatePinnedToCore(usb_host_lib_task, "usb_lib", 4096, self, 5, NULL, 0);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    ESP_ERROR_CHECK(config_init());  // loads NVS-backed bindings (stamps defaults on first boot)

    // AKP USB client must register BEFORE network_init runs. network_init
    // takes ~2.5 s for PHY bring-up; if it ran first, the AKP would
    // enumerate on the USB host during that gap with no registered client
    // listening, the NEW_DEV event would be discarded, and the device
    // would silently stay un-enumerated until the user replugged it.
    ddc_init();   // failures are logged inside; missing monitors don't block boot
    ESP_ERROR_CHECK(key_anim_init());
    ESP_ERROR_CHECK(actions_init());

    lv_init();
    lv_tick_set_cb(lv_tick_ms);
    ESP_ERROR_CHECK(live_key_init());
    ESP_ERROR_CHECK(clock_key_init());
    ESP_ERROR_CHECK(monitor_render_init());   // binds REND_MONITOR slots

    ESP_ERROR_CHECK(akp03e_init(on_akp_event, NULL));

    // Now safe to bring up wired networking — USB enumeration has either
    // already landed (CONNECTED callback fired) or will land on a client
    // that's listening.
    ESP_ERROR_CHECK(network_init()); // wired Ethernet + mDNS; non-blocking
    ESP_ERROR_CHECK(http_server_start());

    ESP_LOGI(TAG, "ready, waiting for device");
}
