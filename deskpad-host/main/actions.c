#include "actions.h"
#include "config.h"
#include "ddc.h"
#include "key_anim.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "actions";

// Tracks the most-recent KVM target so A_KVM_TOGGLE can flip. 0 = unknown.
static uint8_t s_current_pc = 0;
static QueueHandle_t s_q;

static void do_action(action_t a)
{
    if (a == A_NONE) return;
    ESP_LOGI(TAG, "execute: %s", action_name(a));

    switch (a) {
    case A_PAGE_0: key_anim_set_page(0); return;
    case A_PAGE_1: key_anim_set_page(1); return;
    case A_PAGE_2: key_anim_set_page(2); return;
    case A_KVM_PC1:    ddc_switch_to_pc1(); s_current_pc = 1; return;
    case A_KVM_PC2:    ddc_switch_to_pc2(); s_current_pc = 2; return;
    case A_KVM_TOGGLE:
        if (s_current_pc == 1) { ddc_switch_to_pc2(); s_current_pc = 2; }
        else                   { ddc_switch_to_pc1(); s_current_pc = 1; }
        return;
    case A_MON_A_IN1: ddc_set_vcp(DDC_BUS_A, DDC_VCP_INPUT_SOURCE, DDC_U38_INPUT1); return;
    case A_MON_A_IN2: ddc_set_vcp(DDC_BUS_A, DDC_VCP_INPUT_SOURCE, DDC_U38_INPUT2); return;
    case A_MON_B_IN1: ddc_set_vcp(DDC_BUS_B, DDC_VCP_INPUT_SOURCE, DDC_U24_INPUT1); return;
    case A_MON_B_IN2: ddc_set_vcp(DDC_BUS_B, DDC_VCP_INPUT_SOURCE, DDC_U24_INPUT2); return;
    default:
        // Media / system / encoder actions remain log-only until the HID
        // bridge, MQTT, and system-control subsystems land.
        return;
    }
}

static action_t event_to_action(const akp03e_event_t *ev)
{
    const config_t *cfg = config_get();

    switch (ev->type) {
    case AKP03E_EVT_BUTTON:
        if (!ev->pressed) return A_NONE;             // fire on press only
        if (ev->index < KEY_ANIM_KEY_COUNT) {
            return cfg->pages[key_anim_get_page()].slots[ev->index];
        }
        // Side buttons: hardcoded page switchers (deliberately not in config).
        if (ev->index == 6) return A_PAGE_0;
        if (ev->index == 7) return A_PAGE_1;
        if (ev->index == 8) return A_PAGE_2;
        return A_NONE;
    case AKP03E_EVT_ENCODER_PRESS:
        if (!ev->pressed) return A_NONE;
        if (ev->index >= AKP03E_ENCODER_COUNT) return A_NONE;
        return cfg->encoders[ev->index].press;
    case AKP03E_EVT_ENCODER_TWIST:
        if (ev->index >= AKP03E_ENCODER_COUNT) return A_NONE;
        return ev->twist > 0
               ? cfg->encoders[ev->index].twist_pos
               : cfg->encoders[ev->index].twist_neg;
    default:
        return A_NONE;
    }
}

static void actions_worker(void *arg)
{
    (void)arg;
    action_t a;
    for (;;) {
        if (xQueueReceive(s_q, &a, portMAX_DELAY) == pdTRUE) {
            do_action(a);
        }
    }
}

esp_err_t actions_init(void)
{
    if (s_q) return ESP_OK;
    s_q = xQueueCreate(16, sizeof(action_t));
    if (!s_q) return ESP_ERR_NO_MEM;
    BaseType_t ok = xTaskCreate(actions_worker, "actions", 4096, NULL, 4, NULL);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

void actions_handle(const akp03e_event_t *ev)
{
    if (!s_q) return;
    action_t a = event_to_action(ev);
    if (a == A_NONE) return;
    if (xQueueSend(s_q, &a, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, dropped action %d", a);
    }
}
