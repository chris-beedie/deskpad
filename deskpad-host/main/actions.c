#include "actions.h"
#include "akp03e.h"
#include "config.h"
#include "ddc.h"
#include "ha_discovery.h"
#include "hid_keys.h"
#include "hid_link.h"
#include "host_state.h"
#include "key_anim.h"
#include "mqtt.h"
#include "notify.h"
#include "esp_log.h"
#include "nvs.h"

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdint.h>

static const char *TAG = "actions";

// Active Host lives in host_state.c so renderers (monitor) can subscribe.

// ---------------------------------------------------------------------------
// Queued events — the USB callback enqueues these; the worker task derefs
// into the current config and dispatches the appropriate binding.

typedef enum {
    EV_SLOT_PRESS = 1,
    EV_ENCODER_PRESS,
    EV_ENCODER_TWIST,
    EV_PAGE_SWITCH,     // hard-wired side keys — target page in `index`
} event_kind_t;

typedef struct {
    event_kind_t kind;
    uint8_t      page;    // captured at press time
    uint8_t      index;   // 0..5 for slot, 0..2 for encoder
    int8_t       twist;   // +1 / -1 (encoder twist only)
} action_event_t;

static QueueHandle_t s_q;

// ---------------------------------------------------------------------------
// Binding executors. Phase 3 wires KVM and DDC for real; HID and HA log
// only — they'll come alive in Phase 5 (HID via UART link) and Phase 7
// (MQTT) respectively.

static const char *host_label(host_t h)
{
    switch (h) {
    case HOST_PC1: return "PC1";
    case HOST_PC2: return "PC2";
    default:       return "?";
    }
}

static void exec_kvm_select(host_t target)
{
    if (target == HOST_PC1)      { ddc_switch_to_pc1(); host_state_set_active(HOST_PC1); }
    else if (target == HOST_PC2) { ddc_switch_to_pc2(); host_state_set_active(HOST_PC2); }
}

static void exec_kvm_toggle(void)
{
    host_t cur = host_state_get_active();
    host_t next = (cur == HOST_PC1) ? HOST_PC2 : HOST_PC1;
    ESP_LOGI(TAG, "kvm_toggle: %s -> %s", host_label(cur), host_label(next));
    exec_kvm_select(next);
}

static void exec_ddc(const binding_t *b)
{
    ddc_bus_t bus = (b->ddc_bus == 1) ? DDC_BUS_B : DDC_BUS_A;
    switch (b->ddc_variant) {
    case DDC_VAR_VALUE:
        ddc_set_vcp(bus, (uint8_t)b->ddc_vcp, (uint8_t)b->ddc_value);
        break;
    case DDC_VAR_DELTA: {
        uint8_t cur = 0;
        if (ddc_get_vcp(bus, (uint8_t)b->ddc_vcp, &cur) != ESP_OK) {
            ESP_LOGW(TAG, "ddc: get vcp 0x%02x failed; skipping delta", b->ddc_vcp);
            return;
        }
        int next = (int)cur + b->ddc_delta;
        if (next < 0)   next = 0;
        if (next > 100) next = 100;     // brightness-style clamp; tighten later for non-brightness VCPs
        ddc_set_vcp(bus, (uint8_t)b->ddc_vcp, (uint8_t)next);
        break;
    }
    case DDC_VAR_COMMAND:
        ESP_LOGW(TAG, "ddc command '%s' not yet implemented", b->ddc_command ? b->ddc_command : "(null)");
        break;
    default:
        ESP_LOGW(TAG, "ddc: variant unset (bus=%d vcp=0x%02x)", b->ddc_bus, b->ddc_vcp);
        break;
    }
}

static const char *resolve_ha_entity(const slot_config_t *slot, const binding_t *b)
{
    if (b->ha_entity && *b->ha_entity) return b->ha_entity;
    if (slot && slot->entity && *slot->entity) return slot->entity;
    return NULL;
}

// Dispatch a binding. `slot` may be NULL for encoder bindings or API-
// triggered actions (which carry full binding info on `b`).
void actions_fire(const binding_t *b, const slot_config_t *slot)
{
    switch (b->type) {
    case BIND_NONE:
        return;
    case BIND_KVM_SELECT:
        ESP_LOGI(TAG, "kvm_select -> %s", host_label(b->kvm_target));
        exec_kvm_select(b->kvm_target);
        return;
    case BIND_KVM_TOGGLE:
        exec_kvm_toggle();
        return;
    case BIND_DDC:
        ESP_LOGI(TAG, "ddc bus=%d vcp=0x%02x variant=%d", b->ddc_bus, b->ddc_vcp, (int)b->ddc_variant);
        exec_ddc(b);
        return;
    case BIND_HID_CHORD: {
        if (!b->hid_chord || !*b->hid_chord) { ESP_LOGW(TAG, "hid_chord: empty"); return; }
        uint8_t mods = 0, key = 0;
        if (!hid_keys_parse_chord(b->hid_chord, &mods, &key)) {
            ESP_LOGW(TAG, "hid_chord: unrecognised chord '%s'", b->hid_chord);
            return;
        }
        ESP_LOGI(TAG, "hid chord '%s' -> mods=0x%02x key=0x%02x",
                 b->hid_chord, mods, key);
        if (b->hid_hold) ESP_LOGW(TAG, "hid_chord: hold-style not implemented; tapping");
        hid_link_send_chord_tap(mods, key);
        return;
    }
    case BIND_HID_CONSUMER: {
        if (!b->hid_consumer) { ESP_LOGW(TAG, "hid_consumer: empty"); return; }
        uint16_t usage = hid_keys_consumer_code(b->hid_consumer);
        if (!usage) {
            ESP_LOGW(TAG, "hid_consumer: unknown '%s'", b->hid_consumer);
            return;
        }
        ESP_LOGI(TAG, "hid consumer '%s' -> 0x%04x", b->hid_consumer, usage);
        hid_link_send_consumer_tap(usage);
        return;
    }
    case BIND_HA: {
        const char *entity = resolve_ha_entity(slot, b);
        if (!b->ha_service || !*b->ha_service || !entity) {
            ESP_LOGW(TAG, "ha: missing service or entity");
            return;
        }
        // Publish as a single intent on deskpad/cmd; user's HA automation
        // dispatches to the matching service call.
        char payload[192];
        snprintf(payload, sizeof(payload),
                 "{\"service\":\"%s\",\"entity\":\"%s\"}",
                 b->ha_service, entity);
        ESP_LOGI(TAG, "ha -> %s", payload);
        esp_err_t err = mqtt_publish("deskpad/cmd", payload, false);
        if (err != ESP_OK) ESP_LOGW(TAG, "mqtt_publish: %s", esp_err_to_name(err));
        return;
    }
    }
}

// ---------------------------------------------------------------------------
// Scope filter

static bool scope_active(scope_t scope, host_t active)
{
    switch (scope) {
    case SCOPE_GLOBAL:   return true;
    case SCOPE_HOST_PC1: return active == HOST_PC1;
    case SCOPE_HOST_PC2: return active == HOST_PC2;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Worker

static void dispatch_event(const action_event_t *ev)
{
    const config_t *cfg = config_get();

    if (ev->kind == EV_PAGE_SWITCH) {
        if (ev->index < KEY_ANIM_PAGE_COUNT) key_anim_set_page(ev->index);
        return;
    }
    if (ev->page >= KEY_ANIM_PAGE_COUNT) return;

    switch (ev->kind) {
    case EV_SLOT_PRESS: {
        if (ev->index >= KEY_ANIM_KEY_COUNT) return;
        if (notify_consume_press(ev->index)) return;   // press dismissed a notification; don't fire binding
        const slot_config_t *slot = &cfg->pages[ev->page].slots[ev->index];
        host_t active = host_state_get_active();
        if (!scope_active(slot->scope, active)) {
            ESP_LOGD(TAG, "scope-skip page=%u slot=%u scope=%s active=%s",
                     ev->page, ev->index, scope_name(slot->scope), host_label(active));
            return;
        }
        actions_fire(&slot->binding, slot);
        return;
    }
    case EV_ENCODER_PRESS: {
        if (ev->index >= AKP03E_ENCODER_COUNT) return;
        actions_fire(&cfg->encoders[ev->index].pages[ev->page].press, NULL);
        return;
    }
    case EV_ENCODER_TWIST: {
        if (ev->index >= AKP03E_ENCODER_COUNT) return;
        const binding_t *b = (ev->twist > 0)
                             ? &cfg->encoders[ev->index].pages[ev->page].twist_pos
                             : &cfg->encoders[ev->index].pages[ev->page].twist_neg;
        actions_fire(b, NULL);
        return;
    }
    case EV_PAGE_SWITCH:
        return;   // handled above
    }
}

static void actions_worker(void *arg)
{
    (void)arg;
    action_event_t ev;
    for (;;) {
        if (xQueueReceive(s_q, &ev, portMAX_DELAY) == pdTRUE) dispatch_event(&ev);
    }
}

// ---------------------------------------------------------------------------
// AKP brightness (NVS-persisted)

#define NVS_NAMESPACE     "deskpad"
#define NVS_KEY_BRIGHT    "akp_bright"
#define BRIGHT_DEFAULT    70

uint8_t actions_brightness_get(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return BRIGHT_DEFAULT;
    uint8_t v = BRIGHT_DEFAULT;
    nvs_get_u8(h, NVS_KEY_BRIGHT, &v);
    nvs_close(h);
    return v;
}

void actions_brightness_set(uint8_t percent)
{
    if (percent > 100) percent = 100;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_BRIGHT, percent);
        nvs_commit(h);
        nvs_close(h);
    }
    // Best-effort push to the AKP — ignored if not yet attached.
    akp03e_set_brightness(percent);
}

// ---------------------------------------------------------------------------
// Public API

esp_err_t actions_init(void)
{
    if (s_q) return ESP_OK;
    s_q = xQueueCreate(16, sizeof(action_event_t));
    if (!s_q) return ESP_ERR_NO_MEM;
    BaseType_t ok = xTaskCreate(actions_worker, "actions", 4096, NULL, 4, NULL);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

void actions_handle(const akp03e_event_t *ev)
{
    if (!s_q) return;
    action_event_t out = { .page = key_anim_get_page() };

    switch (ev->type) {
    case AKP03E_EVT_BUTTON:
        if (!ev->pressed) return;                          // fire on press only
        if (ev->index < KEY_ANIM_KEY_COUNT) {
            out.kind = EV_SLOT_PRESS;
            out.index = ev->index;
            ha_publish_press_slot(out.page, out.index);
            break;
        }
        // Side keys 6/7/8: hard-wired page switchers. Routed through the
        // worker so key_anim_set_page (which calls akp03e_send_out under
        // the hood) doesn't run on the USB callback task.
        if (ev->index >= 6 && ev->index <= 8) {
            out.kind  = EV_PAGE_SWITCH;
            out.index = ev->index - 6;
            ha_publish_press_side(out.index);
            break;
        }
        return;
    case AKP03E_EVT_ENCODER_PRESS:
        if (!ev->pressed) return;
        out.kind  = EV_ENCODER_PRESS;
        out.index = ev->index;
        ha_publish_press_encoder(out.index);
        break;
    case AKP03E_EVT_ENCODER_TWIST:
        out.kind  = EV_ENCODER_TWIST;
        out.index = ev->index;
        out.twist = ev->twist;
        ha_publish_twist_encoder(out.index, out.twist);
        break;
    default:
        return;
    }

    if (xQueueSend(s_q, &out, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, dropped event kind=%d", out.kind);
    }
}
