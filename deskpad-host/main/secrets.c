#include "secrets.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "secrets";

#define NVS_NAMESPACE  "deskpad"
#define NVS_KEY        "secrets"

static secrets_t s_secrets;
const secrets_t *secrets_get(void) { return &s_secrets; }

static esp_err_t load_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return ESP_OK;  // missing is OK

    size_t len = 0;
    if (nvs_get_str(h, NVS_KEY, NULL, &len) != ESP_OK || len == 0) {
        nvs_close(h); return ESP_OK;
    }
    char *buf = malloc(len);
    if (!buf) { nvs_close(h); return ESP_ERR_NO_MEM; }
    if (nvs_get_str(h, NVS_KEY, buf, &len) != ESP_OK) {
        free(buf); nvs_close(h); return ESP_FAIL;
    }
    nvs_close(h);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return ESP_ERR_INVALID_ARG;

    cJSON *mqtt = cJSON_GetObjectItem(root, "mqtt");
    if (cJSON_IsObject(mqtt)) {
        cJSON *v;
        if ((v = cJSON_GetObjectItem(mqtt, "host")) && cJSON_IsString(v))
            strncpy(s_secrets.mqtt_host, v->valuestring, SECRETS_MQTT_HOST_MAX - 1);
        if ((v = cJSON_GetObjectItem(mqtt, "port")) && cJSON_IsNumber(v))
            s_secrets.mqtt_port = (uint16_t)v->valueint;
        if ((v = cJSON_GetObjectItem(mqtt, "user")) && cJSON_IsString(v))
            strncpy(s_secrets.mqtt_user, v->valuestring, SECRETS_MQTT_USER_MAX - 1);
        if ((v = cJSON_GetObjectItem(mqtt, "password")) && cJSON_IsString(v))
            strncpy(s_secrets.mqtt_pass, v->valuestring, SECRETS_MQTT_PASS_MAX - 1);
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t save_to_nvs(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON *mqtt = cJSON_AddObjectToObject(root, "mqtt");
    cJSON_AddStringToObject(mqtt, "host",     s_secrets.mqtt_host);
    cJSON_AddNumberToObject(mqtt, "port",     s_secrets.mqtt_port);
    cJSON_AddStringToObject(mqtt, "user",     s_secrets.mqtt_user);
    cJSON_AddStringToObject(mqtt, "password", s_secrets.mqtt_pass);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY, json);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    free(json);
    return err;
}

esp_err_t secrets_init(void)
{
    memset(&s_secrets, 0, sizeof(s_secrets));
    s_secrets.mqtt_port = 1883;     // sensible default
    return load_from_nvs();
}

esp_err_t secrets_set_mqtt(const char *host, const uint16_t *port,
                            const char *user, const char *password)
{
    if (host)     { strncpy(s_secrets.mqtt_host, host, SECRETS_MQTT_HOST_MAX - 1);
                    s_secrets.mqtt_host[SECRETS_MQTT_HOST_MAX - 1] = 0; }
    if (port)       s_secrets.mqtt_port = *port;
    if (user)     { strncpy(s_secrets.mqtt_user, user, SECRETS_MQTT_USER_MAX - 1);
                    s_secrets.mqtt_user[SECRETS_MQTT_USER_MAX - 1] = 0; }
    if (password) { strncpy(s_secrets.mqtt_pass, password, SECRETS_MQTT_PASS_MAX - 1);
                    s_secrets.mqtt_pass[SECRETS_MQTT_PASS_MAX - 1] = 0; }
    return save_to_nvs();
}

char *secrets_to_masked_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON *mqtt = cJSON_AddObjectToObject(root, "mqtt");
    cJSON_AddStringToObject(mqtt, "host", s_secrets.mqtt_host);
    cJSON_AddNumberToObject(mqtt, "port", s_secrets.mqtt_port);
    cJSON_AddStringToObject(mqtt, "user", s_secrets.mqtt_user);
    cJSON_AddBoolToObject  (mqtt, "has_password", s_secrets.mqtt_pass[0] != 0);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}
