#include "http_server.h"

#include <string.h>
#include <stdlib.h>

#include "actions.h"
#include "akp03e.h"
#include "cJSON.h"
#include "config.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hid_link.h"
#include "host_state.h"
#include "key_anim.h"
#include "mqtt.h"
#include "notify.h"
#include "secrets.h"
#include "system_status.h"

static const char *TAG = "http";

// SPA — single embedded HTML file. See spa_index.html in the main/ dir.
extern const uint8_t SPA_INDEX_START[] asm("_binary_spa_index_html_start");
extern const uint8_t SPA_INDEX_END[]   asm("_binary_spa_index_html_end");

// ---------------------------------------------------------------------------
// Small helpers

static esp_err_t send_json(httpd_req_t *req, const char *json, esp_err_t status)
{
    httpd_resp_set_type(req, "application/json");
    if (status != ESP_OK) httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, json ? json : "{}");
}

static esp_err_t send_error(httpd_req_t *req, const char *http_status, const char *msg)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, http_status);
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
    return httpd_resp_sendstr(req, buf);
}

// Read the entire request body into a freshly-malloced null-terminated buffer.
// Returns NULL on allocation failure or short read; caller owns the buffer.
static char *read_body(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 64 * 1024) return NULL;     // sanity cap
    char *buf = malloc(total + 1);
    if (!buf) return NULL;

    int read = 0;
    while (read < total) {
        int n = httpd_req_recv(req, buf + read, total - read);
        if (n <= 0) { free(buf); return NULL; }
        read += n;
    }
    buf[total] = '\0';
    return buf;
}

// ---------------------------------------------------------------------------
// Static SPA

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char *)SPA_INDEX_START,
                           SPA_INDEX_END - SPA_INDEX_START);
}

// ---------------------------------------------------------------------------
// /api/config

static esp_err_t handle_get_config(httpd_req_t *req)
{
    char *json = config_to_json();
    if (!json) return send_error(req, "500 Internal Server Error", "serialise failed");
    esp_err_t err = send_json(req, json, ESP_OK);
    free(json);
    return err;
}

static esp_err_t handle_put_config(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) return send_error(req, "400 Bad Request", "missing or oversized body");
    esp_err_t err = config_set_from_json(body);
    free(body);
    if (err != ESP_OK) return send_error(req, "400 Bad Request", "invalid config");
    return send_json(req, "{\"ok\":true}", ESP_OK);
}

// ---------------------------------------------------------------------------
// /api/enums — exposes the enum vocabularies the SPA needs for its
// dropdowns (scope, host, renderer, binding type). Single source of
// truth lives in config.c name tables.

static esp_err_t handle_get_enums(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return send_error(req, "500 Internal Server Error", "alloc");

    static const char * const scopes[]    = { "global", "host:PC1", "host:PC2", NULL };
    static const char * const hosts[]     = { "PC1", "PC2", NULL };
    static const char * const renderers[] = {
        "none", "static_jpeg", "clock", "bulb", "monitor", "thermostat", "text_value", NULL
    };
    static const char * const bindings[]  = {
        "none", "kvm_select", "kvm_toggle", "hid_chord", "hid_consumer", "ha", "ddc", NULL
    };
    const struct { const char *key; const char * const *vals; } groups[] = {
        { "scopes",        scopes    },
        { "hosts",         hosts     },
        { "renderers",     renderers },
        { "binding_types", bindings  },
    };
    for (size_t g = 0; g < sizeof(groups)/sizeof(groups[0]); g++) {
        cJSON *arr = cJSON_AddArrayToObject(root, groups[g].key);
        for (const char * const *p = groups[g].vals; *p; p++)
            cJSON_AddItemToArray(arr, cJSON_CreateString(*p));
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return send_error(req, "500 Internal Server Error", "serialise");
    esp_err_t err = send_json(req, json, ESP_OK);
    free(json);
    return err;
}

// ---------------------------------------------------------------------------
// /api/ota — stream the request body straight into the next OTA slot.

static esp_err_t handle_ota(httpd_req_t *req)
{
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    if (!slot) return send_error(req, "500 Internal Server Error", "no OTA slot");

    ESP_LOGI(TAG, "OTA -> %s (%lu bytes incoming)",
             slot->label, (unsigned long)req->content_len);

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(slot, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_begin: %s", esp_err_to_name(err));
        return send_error(req, "500 Internal Server Error", "ota_begin");
    }

    char buf[4096];
    int remaining = req->content_len;
    while (remaining > 0) {
        int chunk = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int n = httpd_req_recv(req, buf, chunk);
        if (n <= 0) {
            ESP_LOGW(TAG, "ota recv aborted at %d bytes left", remaining);
            esp_ota_abort(handle);
            return send_error(req, "400 Bad Request", "short body");
        }
        err = esp_ota_write(handle, buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota_write: %s", esp_err_to_name(err));
            esp_ota_abort(handle);
            return send_error(req, "500 Internal Server Error", "ota_write");
        }
        remaining -= n;
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_end: %s", esp_err_to_name(err));
        return send_error(req, "400 Bad Request", "image invalid");
    }
    err = esp_ota_set_boot_partition(slot);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition: %s", esp_err_to_name(err));
        return send_error(req, "500 Internal Server Error", "set_boot");
    }

    ESP_LOGI(TAG, "OTA staged — reboot to apply");
    return send_json(req, "{\"ok\":true,\"reboot_required\":true}", ESP_OK);
}

// ---------------------------------------------------------------------------
// /api/state — runtime state snapshot.

static esp_err_t handle_get_state(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return send_error(req, "500 Internal Server Error", "alloc");

    host_t h = host_state_get_active();
    cJSON_AddStringToObject(root, "active_host",
                             h == HOST_PC1 ? "PC1" : h == HOST_PC2 ? "PC2" : "unknown");
    cJSON_AddNumberToObject(root, "current_page", key_anim_get_page());
    cJSON_AddBoolToObject  (root, "hid_link_up",  hid_link_is_up());
    cJSON_AddBoolToObject  (root, "usb_mounted",  hid_link_usb_mounted());
    cJSON_AddBoolToObject  (root, "usb_suspended", hid_link_usb_suspended());
    cJSON_AddBoolToObject  (root, "mqtt_connected", mqtt_is_connected());
    cJSON_AddBoolToObject  (root, "notification_active", notify_overlay_active());
    cJSON_AddNumberToObject(root, "uptime_s", esp_timer_get_time() / 1000000);

    // IP from netif
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            char ipbuf[24];
            snprintf(ipbuf, sizeof(ipbuf), IPSTR, IP2STR(&ip_info.ip));
            cJSON_AddStringToObject(root, "ip", ipbuf);
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return send_error(req, "500 Internal Server Error", "serialise");
    esp_err_t err = send_json(req, json, ESP_OK);
    free(json);
    return err;
}

// ---------------------------------------------------------------------------
// /api/action/* — programmatic triggers (curl-friendly).

static cJSON *parse_body_json(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) return NULL;
    cJSON *root = cJSON_Parse(body);
    free(body);
    return root;
}

static esp_err_t handle_action_kvm_toggle(httpd_req_t *req)
{
    binding_t b = { .type = BIND_KVM_TOGGLE };
    actions_fire(&b, NULL);
    return send_json(req, "{\"ok\":true}", ESP_OK);
}

static esp_err_t handle_action_kvm_select(httpd_req_t *req)
{
    cJSON *root = parse_body_json(req);
    if (!root) return send_error(req, "400 Bad Request", "invalid json");
    cJSON *t = cJSON_GetObjectItem(root, "target");
    binding_t b = { .type = BIND_KVM_SELECT };
    if (cJSON_IsString(t)) {
        if (strcmp(t->valuestring, "PC1") == 0) b.kvm_target = HOST_PC1;
        else if (strcmp(t->valuestring, "PC2") == 0) b.kvm_target = HOST_PC2;
    }
    cJSON_Delete(root);
    if (!b.kvm_target) return send_error(req, "400 Bad Request", "target must be PC1 or PC2");
    actions_fire(&b, NULL);
    return send_json(req, "{\"ok\":true}", ESP_OK);
}

static esp_err_t handle_action_page(httpd_req_t *req)
{
    cJSON *root = parse_body_json(req);
    if (!root) return send_error(req, "400 Bad Request", "invalid json");
    cJSON *p = cJSON_GetObjectItem(root, "page");
    int page = cJSON_IsNumber(p) ? p->valueint : -1;
    cJSON_Delete(root);
    if (page < 0 || page >= KEY_ANIM_PAGE_COUNT)
        return send_error(req, "400 Bad Request", "page out of range");
    key_anim_set_page((uint8_t)page);
    return send_json(req, "{\"ok\":true}", ESP_OK);
}

static esp_err_t handle_action_ddc(httpd_req_t *req)
{
    cJSON *root = parse_body_json(req);
    if (!root) return send_error(req, "400 Bad Request", "invalid json");
    cJSON *jb = cJSON_GetObjectItem(root, "bus");
    cJSON *jv = cJSON_GetObjectItem(root, "vcp");
    cJSON *jval = cJSON_GetObjectItem(root, "value");

    binding_t b = { .type = BIND_DDC };
    b.ddc_bus     = cJSON_IsNumber(jb)  ? jb->valueint  : 0;
    b.ddc_vcp     = cJSON_IsNumber(jv)  ? jv->valueint  : 0;
    b.ddc_variant = DDC_VAR_VALUE;
    b.ddc_value   = cJSON_IsNumber(jval) ? jval->valueint : 0;
    cJSON_Delete(root);

    actions_fire(&b, NULL);
    return send_json(req, "{\"ok\":true}", ESP_OK);
}

static esp_err_t handle_action_hid_chord(httpd_req_t *req)
{
    cJSON *root = parse_body_json(req);
    if (!root) return send_error(req, "400 Bad Request", "invalid json");
    cJSON *c = cJSON_GetObjectItem(root, "chord");
    if (!cJSON_IsString(c)) { cJSON_Delete(root); return send_error(req, "400 Bad Request", "chord required"); }
    binding_t b = { .type = BIND_HID_CHORD, .hid_chord = c->valuestring };
    actions_fire(&b, NULL);
    cJSON_Delete(root);
    return send_json(req, "{\"ok\":true}", ESP_OK);
}

static esp_err_t handle_action_hid_consumer(httpd_req_t *req)
{
    cJSON *root = parse_body_json(req);
    if (!root) return send_error(req, "400 Bad Request", "invalid json");
    cJSON *n = cJSON_GetObjectItem(root, "name");
    if (!cJSON_IsString(n)) { cJSON_Delete(root); return send_error(req, "400 Bad Request", "name required"); }
    binding_t b = { .type = BIND_HID_CONSUMER, .hid_consumer = n->valuestring };
    actions_fire(&b, NULL);
    cJSON_Delete(root);
    return send_json(req, "{\"ok\":true}", ESP_OK);
}

// POST /api/boot_logo — body is a single JPEG (the boot logo).
// Expected dimensions: 240×320 (a 320×240 source pre-rotated +90° CW
// client-side, since the AKP rotates content +90° internally for
// display). The image is persisted to the AKP's flash via the LOG
// opcode and shown at power-on.
static esp_err_t handle_post_boot_logo(httpd_req_t *req)
{
    int total = req->content_len;
    if (total < 4 || total > 0xFFFF) {
        return send_error(req, "400 Bad Request", "body must be a single JPEG ≤ 65535 bytes");
    }
    uint8_t *buf = malloc((size_t)total);
    if (!buf) return send_error(req, "500 Internal Server Error", "alloc");

    int read = 0;
    while (read < total) {
        int n = httpd_req_recv(req, (char *)buf + read, total - read);
        if (n <= 0) { free(buf); return send_error(req, "400 Bad Request", "short body"); }
        read += n;
    }
    if (buf[0] != 0xFF || buf[1] != 0xD8) {
        free(buf);
        return send_error(req, "400 Bad Request", "not a JPEG (missing FFD8 SOI)");
    }

    esp_err_t err = akp03e_set_boot_logo(buf, (size_t)total);
    free(buf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_logo: %s", esp_err_to_name(err));
        return send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    // After a LOG write the AKP keeps the new logo on the LCDs until
    // something pushes fresh key images. Give the flash write ~1s to
    // settle, then redraw the current page so the deskpad UI returns.
    vTaskDelay(pdMS_TO_TICKS(1000));
    key_anim_set_page(key_anim_get_page());
    return send_json(req, "{\"ok\":true,\"note\":\"persisted to AKP flash; power-cycle the AKP to see the new boot logo\"}", ESP_OK);
}

// AKP backlight brightness.
static esp_err_t handle_get_brightness(httpd_req_t *req)
{
    char buf[40];
    snprintf(buf, sizeof(buf), "{\"value\":%u}", actions_brightness_get());
    return send_json(req, buf, ESP_OK);
}

static esp_err_t handle_put_brightness(httpd_req_t *req)
{
    cJSON *root = parse_body_json(req);
    if (!root) return send_error(req, "400 Bad Request", "invalid json");
    cJSON *v = cJSON_GetObjectItem(root, "value");
    if (!cJSON_IsNumber(v)) { cJSON_Delete(root); return send_error(req, "400 Bad Request", "value required"); }
    int pct = v->valueint;
    cJSON_Delete(root);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    actions_brightness_set((uint8_t)pct);
    return send_json(req, "{\"ok\":true}", ESP_OK);
}

// ---------------------------------------------------------------------------
// /api/notifications — list / replace configured notifications.

static esp_err_t handle_get_notifications(httpd_req_t *req)
{
    notification_def_t defs[NOTIF_MAX];
    size_t n = notify_get_all(defs, NOTIF_MAX);

    cJSON *root = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", defs[i].id);
        cJSON_AddNumberToObject(o, "priority", defs[i].priority);
        cJSON *trig = cJSON_AddObjectToObject(o, "trigger");
        cJSON_AddStringToObject(trig, "topic",    defs[i].trigger_topic);
        cJSON_AddStringToObject(trig, "on_value", defs[i].trigger_on_value);
        cJSON *disp = cJSON_AddObjectToObject(o, "display");
        cJSON_AddStringToObject(disp, "style",
                                 defs[i].style == NOTIF_STYLE_OVERLAY ? "overlay" : "takeover");
        cJSON_AddNumberToObject(disp, "target_slot", defs[i].target_slot);
        cJSON_AddStringToObject(disp, "label",       defs[i].label);
        cJSON_AddStringToObject(disp, "colour",      defs[i].colour_hex);
        cJSON_AddNumberToObject(disp, "duration_ms", defs[i].duration_ms);
        cJSON_AddBoolToObject  (disp, "dismiss_any_key",   defs[i].dismiss_any_key);
        cJSON_AddBoolToObject  (disp, "dismiss_source_off", defs[i].dismiss_source_off);
        cJSON_AddItemToArray(root, o);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return send_error(req, "500 Internal Server Error", "serialise");
    esp_err_t err = send_json(req, json, ESP_OK);
    free(json);
    return err;
}

static esp_err_t handle_put_notifications(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) return send_error(req, "400 Bad Request", "missing body");
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request", "expected array");
    }
    int n = cJSON_GetArraySize(root);
    if (n > NOTIF_MAX) n = NOTIF_MAX;
    notification_def_t defs[NOTIF_MAX] = {0};
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_GetArrayItem(root, i);
        if (!cJSON_IsObject(o)) continue;
        cJSON *v;
        if ((v = cJSON_GetObjectItem(o, "id")) && cJSON_IsString(v))
            strncpy(defs[i].id, v->valuestring, NOTIF_ID_MAX - 1);
        if ((v = cJSON_GetObjectItem(o, "priority")) && cJSON_IsNumber(v))
            defs[i].priority = v->valueint;
        cJSON *trig = cJSON_GetObjectItem(o, "trigger");
        if (cJSON_IsObject(trig)) {
            if ((v = cJSON_GetObjectItem(trig, "topic")) && cJSON_IsString(v))
                strncpy(defs[i].trigger_topic, v->valuestring, NOTIF_TOPIC_MAX - 1);
            if ((v = cJSON_GetObjectItem(trig, "on_value")) && cJSON_IsString(v))
                strncpy(defs[i].trigger_on_value, v->valuestring, NOTIF_VALUE_MAX - 1);
        }
        cJSON *disp = cJSON_GetObjectItem(o, "display");
        if (cJSON_IsObject(disp)) {
            if ((v = cJSON_GetObjectItem(disp, "style")) && cJSON_IsString(v))
                defs[i].style = (strcmp(v->valuestring, "overlay") == 0)
                                 ? NOTIF_STYLE_OVERLAY : NOTIF_STYLE_TAKEOVER;
            if ((v = cJSON_GetObjectItem(disp, "target_slot")) && cJSON_IsNumber(v))
                defs[i].target_slot = (uint8_t)v->valueint;
            if ((v = cJSON_GetObjectItem(disp, "label")) && cJSON_IsString(v))
                strncpy(defs[i].label, v->valuestring, NOTIF_LABEL_MAX - 1);
            if ((v = cJSON_GetObjectItem(disp, "colour")) && cJSON_IsString(v))
                strncpy(defs[i].colour_hex, v->valuestring, NOTIF_COLOUR_MAX - 1);
            if ((v = cJSON_GetObjectItem(disp, "duration_ms")) && cJSON_IsNumber(v))
                defs[i].duration_ms = v->valueint;
            if ((v = cJSON_GetObjectItem(disp, "dismiss_any_key")) && cJSON_IsBool(v))
                defs[i].dismiss_any_key = cJSON_IsTrue(v);
            if ((v = cJSON_GetObjectItem(disp, "dismiss_source_off")) && cJSON_IsBool(v))
                defs[i].dismiss_source_off = cJSON_IsTrue(v);
        }
    }
    cJSON_Delete(root);
    esp_err_t err = notify_set_all(defs, (size_t)n);
    if (err != ESP_OK) return send_error(req, "500 Internal Server Error", "save failed");
    return send_json(req, "{\"ok\":true}", ESP_OK);
}

// ---------------------------------------------------------------------------
// /api/health — snapshot of all subsystem statuses.

static esp_err_t handle_get_health(httpd_req_t *req)
{
    status_entry_t entries[MAX_STATUS_ENTRIES];
    size_t n = system_status_get_all(entries, MAX_STATUS_ENTRIES);

    cJSON *root = cJSON_CreateObject();
    if (!root) return send_error(req, "500 Internal Server Error", "alloc");
    cJSON_AddStringToObject(root, "worst",
                             system_status_severity_str(system_status_worst()));
    cJSON *arr = cJSON_AddArrayToObject(root, "entries");
    for (size_t i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "id", entries[i].id);
        cJSON_AddStringToObject(e, "severity",
                                 system_status_severity_str(entries[i].severity));
        cJSON_AddStringToObject(e, "message", entries[i].message);
        cJSON_AddItemToArray(arr, e);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return send_error(req, "500 Internal Server Error", "serialise");
    esp_err_t err = send_json(req, json, ESP_OK);
    free(json);
    return err;
}

// ---------------------------------------------------------------------------
// /api/credentials — GET returns masked secrets; PUT updates them.
// `password` field is omitted from GET (instead, has_password: true|false).
// On PUT, fields not present are left unchanged; password "" clears it.

static esp_err_t handle_get_credentials(httpd_req_t *req)
{
    char *json = secrets_to_masked_json();
    if (!json) return send_error(req, "500 Internal Server Error", "serialise");
    esp_err_t err = send_json(req, json, ESP_OK);
    free(json);
    return err;
}

static esp_err_t handle_put_credentials(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) return send_error(req, "400 Bad Request", "missing body");
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return send_error(req, "400 Bad Request", "invalid json");

    cJSON *mqtt = cJSON_GetObjectItem(root, "mqtt");
    if (!cJSON_IsObject(mqtt)) {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request", "missing mqtt block");
    }
    cJSON *jh = cJSON_GetObjectItem(mqtt, "host");
    cJSON *jp = cJSON_GetObjectItem(mqtt, "port");
    cJSON *ju = cJSON_GetObjectItem(mqtt, "user");
    cJSON *jw = cJSON_GetObjectItem(mqtt, "password");

    const char *host = cJSON_IsString(jh) ? jh->valuestring : NULL;
    const char *user = cJSON_IsString(ju) ? ju->valuestring : NULL;
    const char *pass = cJSON_IsString(jw) ? jw->valuestring : NULL;
    uint16_t port_val = 0;
    const uint16_t *port_ptr = NULL;
    if (cJSON_IsNumber(jp)) { port_val = (uint16_t)jp->valueint; port_ptr = &port_val; }

    esp_err_t err = secrets_set_mqtt(host, port_ptr, user, pass);
    cJSON_Delete(root);
    if (err != ESP_OK) return send_error(req, "500 Internal Server Error", "save failed");

    // Reconnect MQTT with new creds.
    mqtt_reload();
    return send_json(req, "{\"ok\":true}", ESP_OK);
}

// ---------------------------------------------------------------------------
// /api/reboot — schedule an esp_restart() after a small delay so the response
// reaches the client.

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGW(TAG, "rebooting");
    esp_restart();
}

static esp_err_t handle_reboot(httpd_req_t *req)
{
    BaseType_t ok = xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    if (ok != pdPASS) return send_error(req, "500 Internal Server Error", "task create");
    return send_json(req, "{\"ok\":true}", ESP_OK);
}

// ---------------------------------------------------------------------------

esp_err_t http_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    // Override the Kconfig default (CONFIG_HTTPD_MAX_URI_HANDLERS, usually 8).
    // We register ~20 routes; keeping headroom for future endpoints.
    cfg.max_uri_handlers = 32;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) return err;

    static const httpd_uri_t routes[] = {
        { .uri = "/",             .method = HTTP_GET,  .handler = handle_root        },
        { .uri = "/api/config",   .method = HTTP_GET,  .handler = handle_get_config  },
        { .uri = "/api/config",   .method = HTTP_PUT,  .handler = handle_put_config  },
        { .uri = "/api/enums",    .method = HTTP_GET,  .handler = handle_get_enums   },
        { .uri = "/api/state",       .method = HTTP_GET,  .handler = handle_get_state       },
        { .uri = "/api/health",      .method = HTTP_GET,  .handler = handle_get_health      },
        { .uri = "/api/credentials", .method = HTTP_GET,  .handler = handle_get_credentials },
        { .uri = "/api/credentials", .method = HTTP_PUT,  .handler = handle_put_credentials },
        { .uri = "/api/notifications", .method = HTTP_GET, .handler = handle_get_notifications },
        { .uri = "/api/notifications", .method = HTTP_PUT, .handler = handle_put_notifications },
        { .uri = "/api/action/kvm/toggle",   .method = HTTP_POST, .handler = handle_action_kvm_toggle   },
        { .uri = "/api/action/kvm/select",   .method = HTTP_POST, .handler = handle_action_kvm_select   },
        { .uri = "/api/action/page",         .method = HTTP_POST, .handler = handle_action_page         },
        { .uri = "/api/action/ddc",          .method = HTTP_POST, .handler = handle_action_ddc          },
        { .uri = "/api/action/hid/chord",    .method = HTTP_POST, .handler = handle_action_hid_chord    },
        { .uri = "/api/action/hid/consumer", .method = HTTP_POST, .handler = handle_action_hid_consumer },
        { .uri = "/api/brightness",          .method = HTTP_GET,  .handler = handle_get_brightness      },
        { .uri = "/api/brightness",          .method = HTTP_PUT,  .handler = handle_put_brightness      },
        { .uri = "/api/boot_logo",           .method = HTTP_POST, .handler = handle_post_boot_logo      },
        { .uri = "/api/ota",      .method = HTTP_POST, .handler = handle_ota         },
        { .uri = "/api/reboot",   .method = HTTP_POST, .handler = handle_reboot      },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    }

    ESP_LOGI(TAG, "listening on :80");
    return ESP_OK;
}
