#include "http_server.h"

#include <string.h>
#include <stdlib.h>

#include "cJSON.h"
#include "config.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) return err;

    static const httpd_uri_t routes[] = {
        { .uri = "/",             .method = HTTP_GET,  .handler = handle_root        },
        { .uri = "/api/config",   .method = HTTP_GET,  .handler = handle_get_config  },
        { .uri = "/api/config",   .method = HTTP_PUT,  .handler = handle_put_config  },
        { .uri = "/api/enums",    .method = HTTP_GET,  .handler = handle_get_enums   },
        { .uri = "/api/ota",      .method = HTTP_POST, .handler = handle_ota         },
        { .uri = "/api/reboot",   .method = HTTP_POST, .handler = handle_reboot      },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    }

    ESP_LOGI(TAG, "listening on :80");
    return ESP_OK;
}
