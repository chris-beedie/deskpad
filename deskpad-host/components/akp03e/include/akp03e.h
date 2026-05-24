#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Ajazz AKP03E rev 2 driver for ESP32-P4 USB Host.
//
// Layout: 6 LCD keys (indexed 0..5), 3 non-LCD keys (indexed 6..8), 3 encoders (0..2).
// LCD images are 60x60 JPEG, rotated 90 degrees by the device.

#define AKP03E_LCD_KEY_COUNT     6
#define AKP03E_TOTAL_KEY_COUNT   9
#define AKP03E_ENCODER_COUNT     3
#define AKP03E_IMAGE_SIZE        60

typedef enum {
    AKP03E_EVT_CONNECTED,
    AKP03E_EVT_DISCONNECTED,
    AKP03E_EVT_BUTTON,         // .index = 0..8, .pressed = true/false
    AKP03E_EVT_ENCODER_PRESS,  // .index = 0..2, .pressed = true/false
    AKP03E_EVT_ENCODER_TWIST,  // .index = 0..2, .twist = -1 or +1
} akp03e_event_type_t;

typedef struct {
    akp03e_event_type_t type;
    uint8_t index;
    bool pressed;
    int8_t twist;
} akp03e_event_t;

typedef void (*akp03e_event_cb_t)(const akp03e_event_t *event, void *user);

// Registers as a USB Host client and starts the driver task.
// Safe to call once. usb_host_install() must already have been called.
esp_err_t akp03e_init(akp03e_event_cb_t cb, void *user);

// All commands below are no-ops until a device has been enumerated and the
// AKP03E_EVT_CONNECTED event has fired.
esp_err_t akp03e_set_brightness(uint8_t percent);             // 0..100
esp_err_t akp03e_clear_key(uint8_t lcd_key);                  // 0..5
esp_err_t akp03e_clear_all_keys(void);
esp_err_t akp03e_set_key_jpeg(uint8_t lcd_key,
                              const uint8_t *jpeg, size_t len); // raw JPEG bytes
esp_err_t akp03e_sleep(void);
esp_err_t akp03e_shutdown(void);

#ifdef __cplusplus
}
#endif
