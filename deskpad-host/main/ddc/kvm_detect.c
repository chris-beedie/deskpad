#include "kvm_detect.h"

#include "config.h"
#include "ddc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hid_link.h"
#include "host_state.h"

static const char *TAG = "kvm_detect";

#define POLL_INTERVAL_MS  5000
#define CHECK_QUEUE_DEPTH 4

typedef enum {
    REASON_USB_REENUM = 1,
    REASON_DDC_POLL,
} check_reason_t;

static QueueHandle_t s_check_q;

// Map a U38 input value back to a Host (PC1 / PC2) using the configured
// topology. Returns 0 if the value doesn't match either configured input.
static host_t input_to_host(uint8_t u38_input)
{
    const config_t *cfg = config_get();
    if (!cfg) return 0;
    if (u38_input == cfg->kvm.monitor_a.input_pc1) return HOST_PC1;
    if (u38_input == cfg->kvm.monitor_a.input_pc2) return HOST_PC2;
    return 0;
}

// Drive the Secondary Monitor to the input matching `target_host`.
static void follow_secondary(host_t target_host)
{
    const config_t *cfg = config_get();
    if (!cfg) return;
    uint8_t value = (target_host == HOST_PC1) ? cfg->kvm.monitor_b.input_pc1
                                              : cfg->kvm.monitor_b.input_pc2;
    ddc_set_vcp(DDC_BUS_B, DDC_VCP_INPUT_SOURCE, value);
}

static void check_now(check_reason_t reason)
{
    uint8_t cur = 0;
    esp_err_t err = ddc_get_vcp(DDC_BUS_A, DDC_VCP_INPUT_SOURCE, &cur);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ddc_get_vcp(A, 0x60): %s (reason=%d)", esp_err_to_name(err), reason);
        return;
    }

    host_t observed = input_to_host(cur);
    if (observed == 0) {
        ESP_LOGW(TAG, "U38 reports input 0x%02x — not a configured PC1/PC2 value", cur);
        return;
    }

    host_t active = host_state_get_active();
    if (observed == active) return;   // nothing to do

    ESP_LOGI(TAG, "external switch detected (reason=%d): %s -> %s",
             reason,
             active == HOST_PC1 ? "PC1" : active == HOST_PC2 ? "PC2" : "?",
             observed == HOST_PC1 ? "PC1" : "PC2");

    host_state_set_active(observed);
    follow_secondary(observed);
}

// ---------------------------------------------------------------------------
// Triggers — push events onto the queue from any context.

static void on_usb_status(bool mounted, bool suspended, void *user)
{
    (void)suspended; (void)user;
    if (!mounted) return;                 // only react to the re-attach edge
    check_reason_t r = REASON_USB_REENUM;
    xQueueSend(s_check_q, &r, 0);
}

static void on_poll_tick(void *arg)
{
    (void)arg;
    check_reason_t r = REASON_DDC_POLL;
    xQueueSend(s_check_q, &r, 0);
}

// ---------------------------------------------------------------------------
// Worker

static void worker(void *arg)
{
    (void)arg;
    check_reason_t reason;
    for (;;) {
        if (xQueueReceive(s_check_q, &reason, portMAX_DELAY) == pdTRUE) check_now(reason);
    }
}

esp_err_t kvm_detect_init(void)
{
    if (s_check_q) return ESP_OK;
    s_check_q = xQueueCreate(CHECK_QUEUE_DEPTH, sizeof(check_reason_t));
    if (!s_check_q) return ESP_ERR_NO_MEM;

    BaseType_t ok = xTaskCreate(worker, "kvm_detect", 4096, NULL, 4, NULL);
    if (ok != pdPASS) return ESP_FAIL;

    hid_link_set_usb_status_cb(on_usb_status, NULL);

    const esp_timer_create_args_t args = {
        .callback = on_poll_tick,
        .name = "kvm_detect_poll",
    };
    esp_timer_handle_t timer;
    esp_err_t err = esp_timer_create(&args, &timer);
    if (err != ESP_OK) return err;
    err = esp_timer_start_periodic(timer, POLL_INTERVAL_MS * 1000);   // µs
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "init: USB-reenum trigger + %d ms DDC poll", POLL_INTERVAL_MS);
    return ESP_OK;
}
