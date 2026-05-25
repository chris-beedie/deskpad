#include "ha_discovery.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "ddc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hid_link.h"
#include "host_state.h"
#include "key_anim.h"
#include "mqtt.h"

static const char *TAG = "ha_disco";

#define DISCOVERY_PREFIX "homeassistant"
#define DEVICE_ID        "deskpad"
#define BASE_TOPIC       "deskpad"
#define REPUBLISH_MS     5000

// Common device block used in every discovery payload — groups all
// deskpad entities under one HA device.
#define DEVICE_BLOCK                                                  \
    "\"device\":{"                                                     \
    "\"identifiers\":[\"" DEVICE_ID "\"],"                             \
    "\"name\":\"Deskpad\","                                            \
    "\"manufacturer\":\"DIY\","                                        \
    "\"model\":\"deskpad-host (ESP32-P4-NANO)\""                       \
    "}"

// ---------------------------------------------------------------------------
// Discovery — published once on boot and on every reconnect (retained).

typedef struct {
    const char *uid;          // unique_id within deskpad (suffix)
    const char *name;         // HA display name
    const char *state_topic;  // where deskpad publishes the value
} sensor_def_t;

static const sensor_def_t SENSORS[] = {
    { "active_host",  "Active Host",        BASE_TOPIC "/state/active_host"  },
    { "current_page", "Current Page",       BASE_TOPIC "/state/current_page" },
    { "hid_link",     "HID Link",           BASE_TOPIC "/state/hid_link"     },
    { "usb_mounted",  "RP2350 USB Mounted", BASE_TOPIC "/state/usb_mounted"  },
    { "ddc_a",        "DDC Bus A",          BASE_TOPIC "/state/ddc_a"        },
    { "ddc_b",        "DDC Bus B",          BASE_TOPIC "/state/ddc_b"        },
};
#define SENSOR_COUNT (sizeof(SENSORS)/sizeof(SENSORS[0]))

static void publish_discovery(void)
{
    char topic[160];
    char payload[320];
    for (size_t i = 0; i < SENSOR_COUNT; i++) {
        snprintf(topic, sizeof(topic),
                 DISCOVERY_PREFIX "/sensor/" DEVICE_ID "/%s/config", SENSORS[i].uid);
        snprintf(payload, sizeof(payload),
                 "{"
                 "\"name\":\"%s\","
                 "\"state_topic\":\"%s\","
                 "\"unique_id\":\"" DEVICE_ID "_%s\","
                 DEVICE_BLOCK
                 "}",
                 SENSORS[i].name, SENSORS[i].state_topic, SENSORS[i].uid);
        esp_err_t err = mqtt_publish(topic, payload, true);
        if (err != ESP_OK) ESP_LOGW(TAG, "discovery %s: %s", SENSORS[i].uid, esp_err_to_name(err));
    }
}

// ---------------------------------------------------------------------------
// State publishes

static const char *host_to_str(host_t h)
{
    switch (h) { case HOST_PC1: return "PC1"; case HOST_PC2: return "PC2"; default: return "unknown"; }
}

static void publish_state(void)
{
    char buf[16];
    mqtt_publish(BASE_TOPIC "/state/active_host", host_to_str(host_state_get_active()), true);

    snprintf(buf, sizeof(buf), "%u", key_anim_get_page());
    mqtt_publish(BASE_TOPIC "/state/current_page", buf, true);

    mqtt_publish(BASE_TOPIC "/state/hid_link",
                  hid_link_is_up() ? "up" : "down", true);
    mqtt_publish(BASE_TOPIC "/state/usb_mounted",
                  hid_link_usb_mounted() ? "true" : "false", true);

    // DDC health probe — try a get_vcp on each bus. Cheap (one I2C round-trip).
    uint8_t v = 0;
    mqtt_publish(BASE_TOPIC "/state/ddc_a",
                  ddc_get_vcp(DDC_BUS_A, DDC_VCP_POWER_MODE, &v) == ESP_OK ? "ok" : "fail", true);
    mqtt_publish(BASE_TOPIC "/state/ddc_b",
                  ddc_get_vcp(DDC_BUS_B, DDC_VCP_POWER_MODE, &v) == ESP_OK ? "ok" : "fail", true);
}

// ---------------------------------------------------------------------------
// Subscribers / hooks

static void on_active_host_change(host_t new_active, void *user)
{
    (void)new_active; (void)user;
    if (mqtt_is_connected()) publish_state();
}

// MQTT connection state. Track edge so we re-publish discovery on (re)connect.
static bool s_was_connected;

static void tick_cb(void *arg)
{
    (void)arg;
    bool now = mqtt_is_connected();
    if (now && !s_was_connected) {
        ESP_LOGI(TAG, "MQTT (re)connected — publishing discovery + state");
        publish_discovery();
    }
    s_was_connected = now;
    if (now) publish_state();
}

// ---------------------------------------------------------------------------
// Event publishes

void ha_publish_press_slot(uint8_t page, uint8_t slot)
{
    char topic[64];
    snprintf(topic, sizeof(topic), BASE_TOPIC "/event/press/p%us%u", page, slot);
    mqtt_publish(topic, "press", false);
}

void ha_publish_press_side(uint8_t which)
{
    char topic[64];
    snprintf(topic, sizeof(topic), BASE_TOPIC "/event/press/side%u", which);
    mqtt_publish(topic, "press", false);
}

void ha_publish_press_encoder(uint8_t encoder)
{
    char topic[64];
    snprintf(topic, sizeof(topic), BASE_TOPIC "/event/encoder/%u/press", encoder);
    mqtt_publish(topic, "press", false);
}

void ha_publish_twist_encoder(uint8_t encoder, int8_t dir)
{
    char topic[64];
    snprintf(topic, sizeof(topic), BASE_TOPIC "/event/encoder/%u/twist", encoder);
    mqtt_publish(topic, dir > 0 ? "+1" : "-1", false);
}

// ---------------------------------------------------------------------------
// Init

esp_err_t ha_discovery_init(void)
{
    host_state_subscribe(on_active_host_change, NULL);

    const esp_timer_create_args_t args = {
        .callback = tick_cb,
        .name = "ha_disco_tick",
    };
    esp_timer_handle_t timer;
    esp_err_t err = esp_timer_create(&args, &timer);
    if (err != ESP_OK) return err;
    return esp_timer_start_periodic(timer, REPUBLISH_MS * 1000);
}
