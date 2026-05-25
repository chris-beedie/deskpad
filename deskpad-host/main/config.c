#include "config.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "ddc.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "config";

#define NVS_NAMESPACE  "deskpad"
#define NVS_KEY_CONFIG "config"

// ---------------------------------------------------------------------------
// Name tables — single source of truth for both directions of lookup.

#define ENTRY(c, n) { (int)(c), n }

typedef struct { int code; const char *name; } enum_entry_t;

static const enum_entry_t SCOPES[] = {
    ENTRY(SCOPE_GLOBAL,  "global"),
    ENTRY(SCOPE_HOST_PC1, "host:PC1"),
    ENTRY(SCOPE_HOST_PC2, "host:PC2"),
};
static const enum_entry_t HOSTS[] = {
    ENTRY(HOST_PC1, "PC1"),
    ENTRY(HOST_PC2, "PC2"),
};
static const enum_entry_t RENDERERS[] = {
    ENTRY(REND_NONE,        "none"),
    ENTRY(REND_STATIC_JPEG, "static_jpeg"),
    ENTRY(REND_CLOCK,       "clock"),
    ENTRY(REND_BULB,        "bulb"),
    ENTRY(REND_MONITOR,     "monitor"),
    ENTRY(REND_THERMOSTAT,  "thermostat"),
    ENTRY(REND_TEXT_VALUE,  "text_value"),
};
static const enum_entry_t BINDING_TYPES[] = {
    ENTRY(BIND_NONE,         "none"),
    ENTRY(BIND_KVM_SELECT,   "kvm_select"),
    ENTRY(BIND_KVM_TOGGLE,   "kvm_toggle"),
    ENTRY(BIND_HID_CHORD,    "hid_chord"),
    ENTRY(BIND_HID_CONSUMER, "hid_consumer"),
    ENTRY(BIND_HA,           "ha"),
    ENTRY(BIND_DDC,          "ddc"),
};
// Macro parameter names deliberately avoid the struct's `code` and `name`
// fields — earlier names collided with the field accesses after substitution.
#define LOOKUP_NAME(tbl, val, dflt)                                  \
    do {                                                             \
        for (size_t i = 0; i < sizeof(tbl)/sizeof((tbl)[0]); i++)    \
            if ((tbl)[i].code == (int)(val)) return (tbl)[i].name;   \
        return (dflt);                                               \
    } while (0)

#define LOOKUP_CODE(tbl, str, dflt)                                  \
    do {                                                             \
        if (!(str)) return (dflt);                                   \
        for (size_t i = 0; i < sizeof(tbl)/sizeof((tbl)[0]); i++)    \
            if (strcmp((tbl)[i].name, (str)) == 0)                   \
                return (typeof(dflt))(tbl)[i].code;                  \
        return (dflt);                                               \
    } while (0)

const char  *scope_name(scope_t s)         { LOOKUP_NAME(SCOPES,         s, "global"); }
scope_t      scope_from_name(const char *n) { LOOKUP_CODE(SCOPES,         n, SCOPE_GLOBAL); }
const char  *host_name(host_t h)           { LOOKUP_NAME(HOSTS,          h, "PC1"); }
host_t       host_from_name(const char *n)  { LOOKUP_CODE(HOSTS,          n, HOST_PC1); }
const char  *renderer_name(renderer_id_t r) { LOOKUP_NAME(RENDERERS,      r, "none"); }
renderer_id_t renderer_from_name(const char *n) { LOOKUP_CODE(RENDERERS,  n, REND_NONE); }
const char  *binding_type_name(binding_type_t t) { LOOKUP_NAME(BINDING_TYPES, t, "none"); }
binding_type_t binding_type_from_name(const char *n) { LOOKUP_CODE(BINDING_TYPES, n, BIND_NONE); }

// DDC variant lookups intentionally omitted — variant is currently inferred
// from JSON field presence in parse_binding(), not from an explicit string.

// ---------------------------------------------------------------------------
// Heap helpers — every char* field is owned. Replacing a config frees the
// old strings. NULL is permitted everywhere.

static char *strdup_or_null(const char *s)
{
    if (!s || !*s) return NULL;
    char *out = strdup(s);
    return out;
}

#define FREE_AND_NULL(p) do { free(p); (p) = NULL; } while (0)

static void renderer_config_free(renderer_config_t *r)
{
    FREE_AND_NULL(r->label);
    FREE_AND_NULL(r->image);
    FREE_AND_NULL(r->state_topic);
    FREE_AND_NULL(r->on_value);
    FREE_AND_NULL(r->current_topic);
    FREE_AND_NULL(r->target_topic);
    FREE_AND_NULL(r->boost_remaining_topic);
    FREE_AND_NULL(r->unit);
}

static void binding_free(binding_t *b)
{
    FREE_AND_NULL(b->hid_chord);
    FREE_AND_NULL(b->hid_consumer);
    FREE_AND_NULL(b->ha_service);
    FREE_AND_NULL(b->ha_entity);
    FREE_AND_NULL(b->ddc_command);
}

static void slot_free(slot_config_t *s)
{
    FREE_AND_NULL(s->entity);
    renderer_config_free(&s->display);
    binding_free(&s->binding);
}

static void config_free_inner(config_t *c)
{
    for (int p = 0; p < KEY_ANIM_PAGE_COUNT; p++)
        for (int s = 0; s < KEY_ANIM_KEY_COUNT; s++)
            slot_free(&c->pages[p].slots[s]);
    for (int e = 0; e < AKP03E_ENCODER_COUNT; e++)
        for (int p = 0; p < KEY_ANIM_PAGE_COUNT; p++) {
            binding_free(&c->encoders[e].pages[p].press);
            binding_free(&c->encoders[e].pages[p].twist_pos);
            binding_free(&c->encoders[e].pages[p].twist_neg);
        }
}

// ---------------------------------------------------------------------------
// In-memory state

static config_t s_config;
const config_t *config_get(void) { return &s_config; }

// ---------------------------------------------------------------------------
// JSON serialise

static void add_str_if_set(cJSON *o, const char *k, const char *v)
{
    if (v && *v) cJSON_AddStringToObject(o, k, v);
}

static cJSON *renderer_config_to_cjson(const renderer_config_t *r)
{
    cJSON *o = cJSON_CreateObject();
    add_str_if_set(o, "label",                 r->label);
    add_str_if_set(o, "image",                 r->image);
    add_str_if_set(o, "state_topic",           r->state_topic);
    add_str_if_set(o, "on_value",              r->on_value);
    add_str_if_set(o, "current_topic",         r->current_topic);
    add_str_if_set(o, "target_topic",          r->target_topic);
    add_str_if_set(o, "boost_remaining_topic", r->boost_remaining_topic);
    add_str_if_set(o, "unit",                  r->unit);
    if (r->lit_when) cJSON_AddStringToObject(o, "lit_when", host_name(r->lit_when));
    return o;
}

static cJSON *binding_to_cjson(const binding_t *b)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", binding_type_name(b->type));
    switch (b->type) {
    case BIND_KVM_SELECT:
        cJSON_AddStringToObject(o, "target", host_name(b->kvm_target));
        break;
    case BIND_HID_CHORD:
        add_str_if_set(o, "chord", b->hid_chord);
        if (b->hid_hold) cJSON_AddBoolToObject(o, "hold", true);
        break;
    case BIND_HID_CONSUMER:
        add_str_if_set(o, "consumer", b->hid_consumer);
        break;
    case BIND_HA:
        add_str_if_set(o, "service", b->ha_service);
        add_str_if_set(o, "entity",  b->ha_entity);
        break;
    case BIND_DDC:
        cJSON_AddNumberToObject(o, "bus", b->ddc_bus);
        cJSON_AddNumberToObject(o, "vcp", b->ddc_vcp);
        switch (b->ddc_variant) {
        case DDC_VAR_DELTA:   cJSON_AddNumberToObject(o, "delta", b->ddc_delta); break;
        case DDC_VAR_VALUE:   cJSON_AddNumberToObject(o, "value", b->ddc_value); break;
        case DDC_VAR_COMMAND: add_str_if_set(o, "command", b->ddc_command);     break;
        default: break;
        }
        break;
    default: break;
    }
    return o;
}

static cJSON *slot_to_cjson(const slot_config_t *s)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "scope", scope_name(s->scope));
    add_str_if_set(o, "entity", s->entity);
    cJSON *disp = cJSON_AddObjectToObject(o, "display");
    cJSON_AddStringToObject(disp, "renderer", renderer_name(s->renderer));
    cJSON_AddItemToObject(disp, "config", renderer_config_to_cjson(&s->display));
    cJSON_AddItemToObject(o, "binding", binding_to_cjson(&s->binding));
    return o;
}

static cJSON *kvm_to_cjson(const kvm_config_t *k)
{
    cJSON *o = cJSON_CreateObject();
    for (int i = 0; i < 2; i++) {
        const monitor_config_t *m = (i == 0) ? &k->monitor_a : &k->monitor_b;
        cJSON *mo = cJSON_AddObjectToObject(o, i == 0 ? "monitor_a" : "monitor_b");
        cJSON_AddNumberToObject(mo, "ddc_bus", m->ddc_bus);
        cJSON *inputs = cJSON_AddObjectToObject(mo, "inputs");
        cJSON_AddNumberToObject(inputs, "pc1", m->input_pc1);
        cJSON_AddNumberToObject(inputs, "pc2", m->input_pc2);
    }
    return o;
}

char *config_to_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddNumberToObject(root, "version", s_config.version);
    cJSON_AddItemToObject(root, "kvm", kvm_to_cjson(&s_config.kvm));

    cJSON *pages = cJSON_AddArrayToObject(root, "pages");
    for (int p = 0; p < KEY_ANIM_PAGE_COUNT; p++) {
        cJSON *page = cJSON_CreateObject();
        cJSON *slots = cJSON_AddArrayToObject(page, "slots");
        for (int s = 0; s < KEY_ANIM_KEY_COUNT; s++) {
            cJSON_AddItemToArray(slots, slot_to_cjson(&s_config.pages[p].slots[s]));
        }
        cJSON_AddItemToArray(pages, page);
    }

    cJSON *encs = cJSON_AddArrayToObject(root, "encoders");
    for (int e = 0; e < AKP03E_ENCODER_COUNT; e++) {
        cJSON *eo = cJSON_CreateObject();
        cJSON *epages = cJSON_AddArrayToObject(eo, "pages");
        for (int p = 0; p < KEY_ANIM_PAGE_COUNT; p++) {
            cJSON *pg = cJSON_CreateObject();
            cJSON_AddItemToObject(pg, "press",     binding_to_cjson(&s_config.encoders[e].pages[p].press));
            cJSON_AddItemToObject(pg, "twist_pos", binding_to_cjson(&s_config.encoders[e].pages[p].twist_pos));
            cJSON_AddItemToObject(pg, "twist_neg", binding_to_cjson(&s_config.encoders[e].pages[p].twist_neg));
            cJSON_AddItemToArray(epages, pg);
        }
        cJSON_AddItemToArray(encs, eo);
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

// ---------------------------------------------------------------------------
// JSON parse

static const char *get_str(cJSON *o, const char *k)
{
    cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int get_int(cJSON *o, const char *k, int dflt)
{
    cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valueint : dflt;
}

static void parse_renderer_config(cJSON *o, renderer_config_t *out)
{
    if (!cJSON_IsObject(o)) return;
    out->label                 = strdup_or_null(get_str(o, "label"));
    out->image                 = strdup_or_null(get_str(o, "image"));
    out->state_topic           = strdup_or_null(get_str(o, "state_topic"));
    out->on_value              = strdup_or_null(get_str(o, "on_value"));
    out->current_topic         = strdup_or_null(get_str(o, "current_topic"));
    out->target_topic          = strdup_or_null(get_str(o, "target_topic"));
    out->boost_remaining_topic = strdup_or_null(get_str(o, "boost_remaining_topic"));
    out->unit                  = strdup_or_null(get_str(o, "unit"));
    const char *lw = get_str(o, "lit_when");
    if (lw) out->lit_when = host_from_name(lw);
}

static void parse_binding(cJSON *o, binding_t *out)
{
    if (!cJSON_IsObject(o)) { out->type = BIND_NONE; return; }
    out->type = binding_type_from_name(get_str(o, "type"));
    switch (out->type) {
    case BIND_KVM_SELECT:
        out->kvm_target = host_from_name(get_str(o, "target"));
        break;
    case BIND_HID_CHORD:
        out->hid_chord = strdup_or_null(get_str(o, "chord"));
        out->hid_hold  = cJSON_IsTrue(cJSON_GetObjectItem(o, "hold"));
        break;
    case BIND_HID_CONSUMER:
        out->hid_consumer = strdup_or_null(get_str(o, "consumer"));
        break;
    case BIND_HA:
        out->ha_service = strdup_or_null(get_str(o, "service"));
        out->ha_entity  = strdup_or_null(get_str(o, "entity"));
        break;
    case BIND_DDC:
        out->ddc_bus = get_int(o, "bus", 0);
        out->ddc_vcp = get_int(o, "vcp", 0);
        if (cJSON_GetObjectItem(o, "delta")) {
            out->ddc_variant = DDC_VAR_DELTA;
            out->ddc_delta = get_int(o, "delta", 0);
        } else if (cJSON_GetObjectItem(o, "value")) {
            out->ddc_variant = DDC_VAR_VALUE;
            out->ddc_value = get_int(o, "value", 0);
        } else if (get_str(o, "command")) {
            out->ddc_variant = DDC_VAR_COMMAND;
            out->ddc_command = strdup_or_null(get_str(o, "command"));
        }
        break;
    default: break;
    }
}

static void parse_slot(cJSON *o, slot_config_t *out)
{
    if (!cJSON_IsObject(o)) return;
    out->scope  = scope_from_name(get_str(o, "scope"));
    out->entity = strdup_or_null(get_str(o, "entity"));
    cJSON *disp = cJSON_GetObjectItem(o, "display");
    if (cJSON_IsObject(disp)) {
        out->renderer = renderer_from_name(get_str(disp, "renderer"));
        parse_renderer_config(cJSON_GetObjectItem(disp, "config"), &out->display);
    }
    parse_binding(cJSON_GetObjectItem(o, "binding"), &out->binding);
}

static void parse_kvm(cJSON *o, kvm_config_t *out)
{
    if (!cJSON_IsObject(o)) return;
    for (int i = 0; i < 2; i++) {
        cJSON *mo = cJSON_GetObjectItem(o, i == 0 ? "monitor_a" : "monitor_b");
        monitor_config_t *m = (i == 0) ? &out->monitor_a : &out->monitor_b;
        if (!cJSON_IsObject(mo)) continue;
        m->ddc_bus = get_int(mo, "ddc_bus", i);
        cJSON *inputs = cJSON_GetObjectItem(mo, "inputs");
        if (cJSON_IsObject(inputs)) {
            m->input_pc1 = (uint8_t)get_int(inputs, "pc1", m->input_pc1);
            m->input_pc2 = (uint8_t)get_int(inputs, "pc2", m->input_pc2);
        }
    }
}

// Parse a JSON v2 string into a freshly-zeroed config struct. Returns ESP_OK
// on success (caller takes ownership of malloc'd strings inside *out), or
// ESP_ERR_INVALID_ARG and leaves *out partially populated — caller must
// config_free_inner() on failure.
static esp_err_t parse_v2(const char *json, config_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return ESP_ERR_INVALID_ARG;

    esp_err_t err = ESP_OK;
    int ver = get_int(root, "version", -1);
    if (ver != 2) { err = ESP_ERR_INVALID_ARG; goto done; }
    out->version = 2;

    parse_kvm(cJSON_GetObjectItem(root, "kvm"), &out->kvm);

    cJSON *pages = cJSON_GetObjectItem(root, "pages");
    if (!cJSON_IsArray(pages)) { err = ESP_ERR_INVALID_ARG; goto done; }
    for (int p = 0; p < KEY_ANIM_PAGE_COUNT; p++) {
        cJSON *page = cJSON_GetArrayItem(pages, p);
        cJSON *slots = cJSON_GetObjectItem(page, "slots");
        for (int s = 0; s < KEY_ANIM_KEY_COUNT; s++) {
            parse_slot(cJSON_GetArrayItem(slots, s), &out->pages[p].slots[s]);
        }
    }

    cJSON *encs = cJSON_GetObjectItem(root, "encoders");
    if (cJSON_IsArray(encs)) {
        for (int e = 0; e < AKP03E_ENCODER_COUNT; e++) {
            cJSON *eo = cJSON_GetArrayItem(encs, e);
            cJSON *epages = cJSON_GetObjectItem(eo, "pages");
            if (!cJSON_IsArray(epages)) continue;
            for (int p = 0; p < KEY_ANIM_PAGE_COUNT; p++) {
                cJSON *pg = cJSON_GetArrayItem(epages, p);
                parse_binding(cJSON_GetObjectItem(pg, "press"),     &out->encoders[e].pages[p].press);
                parse_binding(cJSON_GetObjectItem(pg, "twist_pos"), &out->encoders[e].pages[p].twist_pos);
                parse_binding(cJSON_GetObjectItem(pg, "twist_neg"), &out->encoders[e].pages[p].twist_neg);
            }
        }
    }

done:
    cJSON_Delete(root);
    return err;
}

// ---------------------------------------------------------------------------
// Defaults — equivalent to the v1 default tables but expressed as v2 records.
// Lifts DDC topology out of the ddc.h #defines and seeds it as configurable.

static slot_config_t default_static(const char *image_name, const char *label)
{
    slot_config_t s = { .scope = SCOPE_GLOBAL, .renderer = REND_STATIC_JPEG };
    s.display.image = strdup_or_null(image_name);
    s.display.label = strdup_or_null(label);
    return s;
}

static slot_config_t default_kvm_select(host_t target, const char *label)
{
    slot_config_t s = { .scope = SCOPE_GLOBAL, .renderer = REND_MONITOR };
    s.display.label    = strdup_or_null(label);
    s.display.lit_when = target;
    s.binding.type = BIND_KVM_SELECT;
    s.binding.kvm_target = target;
    return s;
}

static slot_config_t default_ddc_value(int bus, uint8_t vcp, uint8_t value, const char *label)
{
    slot_config_t s = { .scope = SCOPE_GLOBAL, .renderer = REND_STATIC_JPEG };
    s.display.label = strdup_or_null(label);
    s.binding.type = BIND_DDC;
    s.binding.ddc_bus = bus;
    s.binding.ddc_vcp = vcp;
    s.binding.ddc_variant = DDC_VAR_VALUE;
    s.binding.ddc_value = value;
    return s;
}

static slot_config_t default_hid_consumer(const char *consumer, const char *label)
{
    slot_config_t s = { .scope = SCOPE_GLOBAL, .renderer = REND_STATIC_JPEG };
    s.display.label = strdup_or_null(label);
    s.binding.type = BIND_HID_CONSUMER;
    s.binding.hid_consumer = strdup_or_null(consumer);
    return s;
}

static slot_config_t default_hid_chord(const char *chord, const char *label)
{
    slot_config_t s = { .scope = SCOPE_GLOBAL, .renderer = REND_STATIC_JPEG };
    s.display.label = strdup_or_null(label);
    s.binding.type = BIND_HID_CHORD;
    s.binding.hid_chord = strdup_or_null(chord);
    return s;
}

static binding_t default_ddc_delta_binding(int bus, uint8_t vcp, int delta)
{
    binding_t b = { .type = BIND_DDC, .ddc_bus = bus, .ddc_vcp = vcp,
                    .ddc_variant = DDC_VAR_DELTA, .ddc_delta = delta };
    return b;
}

static binding_t default_hid_consumer_binding(const char *consumer)
{
    binding_t b = { .type = BIND_HID_CONSUMER };
    b.hid_consumer = strdup_or_null(consumer);
    return b;
}

static void build_defaults(config_t *out)
{
    out->version = CONFIG_SCHEMA_VERSION;
    out->kvm.monitor_a = (monitor_config_t){
        .ddc_bus   = DDC_BUS_A,
        .input_pc1 = DDC_U38_INPUT1,
        .input_pc2 = DDC_U38_INPUT2,
    };
    out->kvm.monitor_b = (monitor_config_t){
        .ddc_bus   = DDC_BUS_B,
        .input_pc1 = DDC_U24_INPUT1,
        .input_pc2 = DDC_U24_INPUT2,
    };

    // Page 0 — KVM & monitor input controls (carried over from v1 defaults)
    out->pages[0].slots[0] = default_kvm_select(HOST_PC1, "Work");
    out->pages[0].slots[1] = default_kvm_select(HOST_PC2, "Home");
    out->pages[0].slots[2] = default_ddc_value(DDC_BUS_A, DDC_VCP_INPUT_SOURCE, DDC_U38_INPUT1, "U38 DP");
    out->pages[0].slots[3] = default_ddc_value(DDC_BUS_A, DDC_VCP_INPUT_SOURCE, DDC_U38_INPUT2, "U38 USB-C");
    out->pages[0].slots[4] = default_ddc_value(DDC_BUS_B, DDC_VCP_INPUT_SOURCE, DDC_U24_INPUT1, "U24 HDMI");
    out->pages[0].slots[5] = default_ddc_value(DDC_BUS_B, DDC_VCP_INPUT_SOURCE, DDC_U24_INPUT2, "U24 DP");

    // Page 1 — Media keys (consumer-control HID)
    out->pages[1].slots[0] = default_hid_consumer("PLAY_PAUSE", "Play / Pause");
    out->pages[1].slots[1] = default_hid_consumer("PREV_TRACK", "Prev");
    out->pages[1].slots[2] = default_hid_consumer("NEXT_TRACK", "Next");
    out->pages[1].slots[3] = default_hid_consumer("VOL_UP",     "Vol +");
    out->pages[1].slots[4] = default_hid_consumer("VOL_DOWN",   "Vol -");
    out->pages[1].slots[5] = default_hid_consumer("MUTE",       "Mute");

    // Page 2 — System chords
    out->pages[2].slots[0] = default_hid_chord("win+l",         "Lock");
    out->pages[2].slots[1] = default_static("sleep.jpg",        "Sleep");
    out->pages[2].slots[2] = default_hid_chord("win+shift+s",   "Screenshot");
    out->pages[2].slots[3] = default_hid_chord("win+alt+r",     "Record");
    out->pages[2].slots[4] = default_static("dnd.jpg",          "DND");
    // Slot 5 of page 2 is a clock by default — matches the pre-Phase-4b
    // hardcoded clock position so fresh-flashed devices still show a clock.
    {
        slot_config_t s = { .scope = SCOPE_GLOBAL, .renderer = REND_CLOCK };
        s.display.label = strdup_or_null("Clock");
        out->pages[2].slots[5] = s;
    }

    // Encoder defaults — same binding across all pages.
    for (int p = 0; p < KEY_ANIM_PAGE_COUNT; p++) {
        out->encoders[0].pages[p].press     = default_hid_consumer_binding("MUTE");
        out->encoders[0].pages[p].twist_pos = default_hid_consumer_binding("VOL_UP");
        out->encoders[0].pages[p].twist_neg = default_hid_consumer_binding("VOL_DOWN");
        out->encoders[1].pages[p].twist_pos = default_ddc_delta_binding(DDC_BUS_A, 0x10, +5);
        out->encoders[1].pages[p].twist_neg = default_ddc_delta_binding(DDC_BUS_A, 0x10, -5);
        out->encoders[2].pages[p].twist_pos = default_ddc_delta_binding(DDC_BUS_B, 0x10, +5);
        out->encoders[2].pages[p].twist_neg = default_ddc_delta_binding(DDC_BUS_B, 0x10, -5);
    }
}

// ---------------------------------------------------------------------------
// v1 -> v2 migration. v1 stored an action_t enum per slot; we translate each
// enum value into the equivalent v2 slot. Default kvm topology comes from
// the same place as for a fresh build.

typedef enum {
    A_NONE = 0, A_PAGE_0, A_PAGE_1, A_PAGE_2,
    A_KVM_PC1, A_KVM_PC2, A_KVM_TOGGLE,
    A_MON_A_IN1, A_MON_A_IN2, A_MON_B_IN1, A_MON_B_IN2,
    A_PLAY_PAUSE, A_PREV_TRACK, A_NEXT_TRACK,
    A_VOL_UP, A_VOL_DOWN, A_MUTE,
    A_LOCK, A_SLEEP, A_SCREENSHOT, A_REC, A_DND, A_CALC,
    A_BRIGHT_A_UP, A_BRIGHT_A_DOWN, A_BRIGHT_B_UP, A_BRIGHT_B_DOWN,
} v1_action_t;

static const struct { v1_action_t code; const char *name; } V1_ACTION_NAMES[] = {
    {A_NONE,"NONE"}, {A_PAGE_0,"PAGE_0"}, {A_PAGE_1,"PAGE_1"}, {A_PAGE_2,"PAGE_2"},
    {A_KVM_PC1,"KVM_PC1"}, {A_KVM_PC2,"KVM_PC2"}, {A_KVM_TOGGLE,"KVM_TOGGLE"},
    {A_MON_A_IN1,"MON_A_IN1"}, {A_MON_A_IN2,"MON_A_IN2"},
    {A_MON_B_IN1,"MON_B_IN1"}, {A_MON_B_IN2,"MON_B_IN2"},
    {A_PLAY_PAUSE,"PLAY_PAUSE"}, {A_PREV_TRACK,"PREV_TRACK"}, {A_NEXT_TRACK,"NEXT_TRACK"},
    {A_VOL_UP,"VOL_UP"}, {A_VOL_DOWN,"VOL_DOWN"}, {A_MUTE,"MUTE"},
    {A_LOCK,"LOCK"}, {A_SLEEP,"SLEEP"}, {A_SCREENSHOT,"SCREENSHOT"},
    {A_REC,"REC"}, {A_DND,"DND"}, {A_CALC,"CALC"},
    {A_BRIGHT_A_UP,"BRIGHT_A_UP"}, {A_BRIGHT_A_DOWN,"BRIGHT_A_DOWN"},
    {A_BRIGHT_B_UP,"BRIGHT_B_UP"}, {A_BRIGHT_B_DOWN,"BRIGHT_B_DOWN"},
};

static v1_action_t v1_from_name(const char *n)
{
    if (!n) return A_NONE;
    for (size_t i = 0; i < sizeof(V1_ACTION_NAMES)/sizeof(V1_ACTION_NAMES[0]); i++)
        if (strcmp(V1_ACTION_NAMES[i].name, n) == 0) return V1_ACTION_NAMES[i].code;
    return A_NONE;
}

static slot_config_t v1_to_slot(v1_action_t a, const kvm_config_t *kvm)
{
    switch (a) {
    case A_KVM_PC1:     return default_kvm_select(HOST_PC1, "PC1");
    case A_KVM_PC2:     return default_kvm_select(HOST_PC2, "PC2");
    case A_KVM_TOGGLE: {
        slot_config_t s = { .scope = SCOPE_GLOBAL, .renderer = REND_MONITOR };
        s.display.label = strdup_or_null("KVM");
        s.binding.type = BIND_KVM_TOGGLE;
        return s;
    }
    case A_MON_A_IN1: return default_ddc_value(DDC_BUS_A, DDC_VCP_INPUT_SOURCE, kvm->monitor_a.input_pc1, "U38 in1");
    case A_MON_A_IN2: return default_ddc_value(DDC_BUS_A, DDC_VCP_INPUT_SOURCE, kvm->monitor_a.input_pc2, "U38 in2");
    case A_MON_B_IN1: return default_ddc_value(DDC_BUS_B, DDC_VCP_INPUT_SOURCE, kvm->monitor_b.input_pc1, "U24 in1");
    case A_MON_B_IN2: return default_ddc_value(DDC_BUS_B, DDC_VCP_INPUT_SOURCE, kvm->monitor_b.input_pc2, "U24 in2");
    case A_PLAY_PAUSE: return default_hid_consumer("PLAY_PAUSE", "Play / Pause");
    case A_PREV_TRACK: return default_hid_consumer("PREV_TRACK", "Prev");
    case A_NEXT_TRACK: return default_hid_consumer("NEXT_TRACK", "Next");
    case A_VOL_UP:     return default_hid_consumer("VOL_UP",     "Vol +");
    case A_VOL_DOWN:   return default_hid_consumer("VOL_DOWN",   "Vol -");
    case A_MUTE:       return default_hid_consumer("MUTE",       "Mute");
    case A_LOCK:       return default_hid_chord("win+l",         "Lock");
    case A_SCREENSHOT: return default_hid_chord("win+shift+s",   "Screenshot");
    case A_REC:        return default_hid_chord("win+alt+r",     "Record");
    case A_SLEEP:      return default_static("sleep.jpg", "Sleep");
    case A_DND:        return default_static("dnd.jpg",   "DND");
    case A_CALC:       return default_static("calc.jpg",  "Calc");
    default: {
        slot_config_t s = { .scope = SCOPE_GLOBAL, .renderer = REND_NONE };
        return s;
    }
    }
}

static binding_t v1_to_encoder_binding(v1_action_t a, const kvm_config_t *kvm)
{
    (void)kvm;
    switch (a) {
    case A_VOL_UP:        return default_hid_consumer_binding("VOL_UP");
    case A_VOL_DOWN:      return default_hid_consumer_binding("VOL_DOWN");
    case A_MUTE:          return default_hid_consumer_binding("MUTE");
    case A_BRIGHT_A_UP:   return default_ddc_delta_binding(DDC_BUS_A, 0x10, +5);
    case A_BRIGHT_A_DOWN: return default_ddc_delta_binding(DDC_BUS_A, 0x10, -5);
    case A_BRIGHT_B_UP:   return default_ddc_delta_binding(DDC_BUS_B, 0x10, +5);
    case A_BRIGHT_B_DOWN: return default_ddc_delta_binding(DDC_BUS_B, 0x10, -5);
    default:              return (binding_t){ .type = BIND_NONE };
    }
}

static esp_err_t migrate_v1_to_v2(const char *json, config_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return ESP_ERR_INVALID_ARG;
    if (get_int(root, "version", -1) != 1) { cJSON_Delete(root); return ESP_ERR_INVALID_ARG; }

    ESP_LOGI(TAG, "migrating v1 -> v2");
    build_defaults(out);   // start with v2 defaults, overwrite from v1 data

    cJSON *pages = cJSON_GetObjectItem(root, "pages");
    if (cJSON_IsArray(pages)) {
        for (int p = 0; p < KEY_ANIM_PAGE_COUNT; p++) {
            cJSON *page = cJSON_GetArrayItem(pages, p);
            cJSON *slots = cJSON_GetObjectItem(page, "slots");
            if (!cJSON_IsArray(slots)) continue;
            for (int s = 0; s < KEY_ANIM_KEY_COUNT; s++) {
                const char *aname = cJSON_GetArrayItem(slots, s) ? cJSON_GetArrayItem(slots, s)->valuestring : NULL;
                slot_free(&out->pages[p].slots[s]);
                out->pages[p].slots[s] = v1_to_slot(v1_from_name(aname), &out->kvm);
            }
        }
    }

    cJSON *encs = cJSON_GetObjectItem(root, "encoders");
    if (cJSON_IsArray(encs)) {
        for (int e = 0; e < AKP03E_ENCODER_COUNT; e++) {
            cJSON *enc = cJSON_GetArrayItem(encs, e);
            if (!cJSON_IsObject(enc)) continue;
            v1_action_t press     = v1_from_name(get_str(enc, "press"));
            v1_action_t twist_pos = v1_from_name(get_str(enc, "twist_pos"));
            v1_action_t twist_neg = v1_from_name(get_str(enc, "twist_neg"));
            for (int p = 0; p < KEY_ANIM_PAGE_COUNT; p++) {
                binding_free(&out->encoders[e].pages[p].press);
                binding_free(&out->encoders[e].pages[p].twist_pos);
                binding_free(&out->encoders[e].pages[p].twist_neg);
                out->encoders[e].pages[p].press     = v1_to_encoder_binding(press,     &out->kvm);
                out->encoders[e].pages[p].twist_pos = v1_to_encoder_binding(twist_pos, &out->kvm);
                out->encoders[e].pages[p].twist_neg = v1_to_encoder_binding(twist_neg, &out->kvm);
            }
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// NVS persistence

static esp_err_t nvs_save(const char *json)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, NVS_KEY_CONFIG, json);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static char *nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return NULL;
    size_t len = 0;
    if (nvs_get_str(h, NVS_KEY_CONFIG, NULL, &len) != ESP_OK || len == 0) {
        nvs_close(h); return NULL;
    }
    char *buf = malloc(len);
    if (!buf) { nvs_close(h); return NULL; }
    if (nvs_get_str(h, NVS_KEY_CONFIG, buf, &len) != ESP_OK) {
        free(buf); nvs_close(h); return NULL;
    }
    nvs_close(h);
    return buf;
}

static esp_err_t persist_current(void)
{
    char *json = config_to_json();
    if (!json) return ESP_ERR_NO_MEM;
    esp_err_t err = nvs_save(json);
    free(json);
    return err;
}

esp_err_t config_set_from_json(const char *json)
{
    config_t fresh = {0};
    esp_err_t err = parse_v2(json, &fresh);
    if (err != ESP_OK) { config_free_inner(&fresh); return err; }

    config_free_inner(&s_config);
    s_config = fresh;
    return nvs_save(json);
}

esp_err_t config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs_flash_init: %s — erasing and retrying", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    char *json = nvs_load();
    if (json) {
        // Detect schema version cheaply before full parse.
        cJSON *root = cJSON_Parse(json);
        int ver = root ? get_int(root, "version", -1) : -1;
        cJSON_Delete(root);

        if (ver == 2) {
            if (parse_v2(json, &s_config) == ESP_OK) {
                ESP_LOGI(TAG, "loaded v2 config from NVS");
                free(json);
                return ESP_OK;
            }
            ESP_LOGW(TAG, "v2 parse failed — falling back to defaults");
            config_free_inner(&s_config);
            memset(&s_config, 0, sizeof(s_config));
        } else if (ver == 1) {
            if (migrate_v1_to_v2(json, &s_config) == ESP_OK) {
                ESP_LOGI(TAG, "migrated v1 -> v2");
                free(json);
                return persist_current();
            }
            ESP_LOGW(TAG, "v1 migration failed — falling back to defaults");
            config_free_inner(&s_config);
            memset(&s_config, 0, sizeof(s_config));
        } else {
            ESP_LOGW(TAG, "unknown schema version %d — falling back to defaults", ver);
        }
        free(json);
    } else {
        ESP_LOGI(TAG, "no NVS config — stamping defaults");
    }

    build_defaults(&s_config);
    return persist_current();
}
