#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// DDC/CI driver for two monitors hanging off two independent I2C buses on the
// ESP32-P4-NANO. Slave address is 0x37 (7-bit); both monitors live there.
//
// Default pin assignment below is a sensible starting point but unverified
// against your physical wiring — update before connecting level shifters.

typedef enum {
    DDC_BUS_A = 0,   // Dell U3821DW   ("monitor A")
    DDC_BUS_B = 1,   // Dell U2419H    ("monitor B")
    DDC_BUS_COUNT
} ddc_bus_t;

// VCP feature codes
#define DDC_VCP_INPUT_SOURCE   0x60
#define DDC_VCP_POWER_MODE     0xD6

// Per-monitor input values for VCP 0x60 — carried over from the S3 firmware.
//   Bus A (U3821DW): input1 = DisplayPort (Laptop 1), input2 = USB-C (Laptop 2)
//   Bus B (U2419H):  input1 = HDMI 1     (Laptop 1), input2 = DisplayPort (Laptop 2)
#define DDC_U38_INPUT1   0x0F
#define DDC_U38_INPUT2   0x1B
#define DDC_U24_INPUT1   0x11
#define DDC_U24_INPUT2   0x0F

esp_err_t ddc_init(void);

// Set / get the active input on a monitor. Returns ESP_OK on success, or an
// I2C error code (ESP_ERR_TIMEOUT, etc.) if the monitor didn't ACK.
esp_err_t ddc_set_vcp(ddc_bus_t bus, uint8_t vcp_code, uint8_t value);
esp_err_t ddc_get_vcp(ddc_bus_t bus, uint8_t vcp_code, uint8_t *value);

// Convenience: switch both monitors to "PC1" or "PC2" using the codes above.
esp_err_t ddc_switch_to_pc1(void);
esp_err_t ddc_switch_to_pc2(void);

// True if VCP 0xD6 reports power mode == 0x01 (on).
bool ddc_monitor_awake(ddc_bus_t bus);
