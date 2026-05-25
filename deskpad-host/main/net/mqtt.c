#include "mqtt.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"   // esp-mqtt (IDF) — esp_mqtt_* types
#include "secrets.h"
#include "system_status.h"

static const char *TAG = "mqtt";

#define MAX_SUBS 32

typedef struct {
    bool                 in_use;
    char                *topic;
    mqtt_subscriber_cb_t cb;
    void                *user;
} subscriber_t;

static subscriber_t s_subs[MAX_SUBS];
static SemaphoreHandle_t s_subs_mtx;

static esp_mqtt_client_handle_t s_client;
static bool s_connected;

// ---------------------------------------------------------------------------
// Subscription registry helpers

static int sub_alloc(const char *topic, mqtt_subscriber_cb_t cb, void *user)
{
    xSemaphoreTake(s_subs_mtx, portMAX_DELAY);
    int idx = -1;
    for (int i = 0; i < MAX_SUBS; i++) {
        if (!s_subs[i].in_use) {
            s_subs[i].in_use = true;
            s_subs[i].topic  = strdup(topic);
            s_subs[i].cb     = cb;
            s_subs[i].user   = user;
            idx = i;
            break;
        }
    }
    xSemaphoreGive(s_subs_mtx);
    return idx;
}

static void sub_free(int idx)
{
    if (idx < 0 || idx >= MAX_SUBS) return;
    xSemaphoreTake(s_subs_mtx, portMAX_DELAY);
    if (s_subs[idx].in_use) {
        free(s_subs[idx].topic);
        s_subs[idx].topic = NULL;
        s_subs[idx].in_use = false;
    }
    xSemaphoreGive(s_subs_mtx);
}

static void dispatch_message(const char *topic, size_t topic_len,
                              const char *payload, size_t payload_len)
{
    xSemaphoreTake(s_subs_mtx, portMAX_DELAY);
    for (int i = 0; i < MAX_SUBS; i++) {
        if (!s_subs[i].in_use || !s_subs[i].topic || !s_subs[i].cb) continue;
        // Exact match for now (no wildcard handling in dispatcher).
        if (strlen(s_subs[i].topic) == topic_len &&
            memcmp(s_subs[i].topic, topic, topic_len) == 0) {
            mqtt_subscriber_cb_t cb = s_subs[i].cb;
            void *user = s_subs[i].user;
            xSemaphoreGive(s_subs_mtx);
            cb(s_subs[i].topic, payload, payload_len, user);
            xSemaphoreTake(s_subs_mtx, portMAX_DELAY);
        }
    }
    xSemaphoreGive(s_subs_mtx);
}

static void resubscribe_all(void)
{
    xSemaphoreTake(s_subs_mtx, portMAX_DELAY);
    for (int i = 0; i < MAX_SUBS; i++) {
        if (s_subs[i].in_use && s_subs[i].topic && s_client) {
            esp_mqtt_client_subscribe(s_client, s_subs[i].topic, 0);
        }
    }
    xSemaphoreGive(s_subs_mtx);
}

// ---------------------------------------------------------------------------
// esp-mqtt event handler

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "connected to broker");
        system_status_set("mqtt", STATUS_OK, "connected");
        resubscribe_all();
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "disconnected from broker");
        system_status_set("mqtt", STATUS_WARN, "disconnected");
        break;
    case MQTT_EVENT_DATA:
        if (ev->topic && ev->data) {
            dispatch_message(ev->topic, ev->topic_len, ev->data, ev->data_len);
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "mqtt error: type=%d",
                 ev->error_handle ? ev->error_handle->error_type : -1);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Public API

bool mqtt_is_connected(void) { return s_connected; }

esp_err_t mqtt_init(void)
{
    if (!s_subs_mtx) s_subs_mtx = xSemaphoreCreateMutex();
    return mqtt_reload();
}

esp_err_t mqtt_reload(void)
{
    const secrets_t *s = secrets_get();
    if (!s || s->mqtt_host[0] == 0) {
        ESP_LOGI(TAG, "no MQTT host configured — skipping");
        system_status_set("mqtt", STATUS_WARN, "no broker configured");
        return ESP_OK;
    }

    // Tear down any existing client before reconfiguring.
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        s_connected = false;
    }

    char uri[160];
    snprintf(uri, sizeof(uri), "mqtt://%s:%u", s->mqtt_host, s->mqtt_port);
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .credentials.username = s->mqtt_user[0] ? s->mqtt_user : NULL,
        .credentials.authentication.password = s->mqtt_pass[0] ? s->mqtt_pass : NULL,
        .session.keepalive = 30,
        .network.reconnect_timeout_ms = 5000,
    };
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) return ESP_FAIL;
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, event_handler, NULL));
    return esp_mqtt_client_start(s_client);
}

int mqtt_subscribe(const char *topic, mqtt_subscriber_cb_t cb, void *user)
{
    if (!topic || !cb) return -1;
    int idx = sub_alloc(topic, cb, user);
    if (idx < 0) return -1;
    if (s_client && s_connected) esp_mqtt_client_subscribe(s_client, topic, 0);
    return idx;
}

void mqtt_unsubscribe(int handle)
{
    if (handle < 0 || handle >= MAX_SUBS) return;
    xSemaphoreTake(s_subs_mtx, portMAX_DELAY);
    char *topic = s_subs[handle].topic ? strdup(s_subs[handle].topic) : NULL;
    xSemaphoreGive(s_subs_mtx);

    sub_free(handle);
    if (s_client && s_connected && topic) {
        esp_mqtt_client_unsubscribe(s_client, topic);
    }
    free(topic);
}

esp_err_t mqtt_publish(const char *topic, const char *payload, bool retain)
{
    if (!s_client || !s_connected) return ESP_ERR_INVALID_STATE;
    int msg_id = esp_mqtt_client_publish(s_client, topic, payload,
                                          payload ? strlen(payload) : 0,
                                          0, retain ? 1 : 0);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}
