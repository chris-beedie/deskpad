#include "ddc.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "system_status.h"

#include <string.h>

static const char *TAG = "ddc";

// Sensible defaults — UNVERIFIED against the actual P4-NANO wiring.
// Update these to match your level-shifter / monitor harness.
#define DDC_BUS_A_SDA_GPIO   4
#define DDC_BUS_A_SCL_GPIO   5
#define DDC_BUS_B_SDA_GPIO   6
#define DDC_BUS_B_SCL_GPIO   7

#define DDC_I2C_FREQ_HZ      100000   // DDC/CI mandates 100 kHz max
#define DDC_SLAVE_ADDR       0x37
#define DDC_RETRY_DELAY_MS   100
#define DDC_GET_REPLY_MS     40

static i2c_master_bus_handle_t s_bus[DDC_BUS_COUNT];
static i2c_master_dev_handle_t s_dev[DDC_BUS_COUNT];
static bool s_inited;

static esp_err_t init_one_bus(ddc_bus_t bus, int sda, int scl, int port)
{
    const i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = port,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,  // external level shifter has its
                                               // own pullups; internal pull is
                                               // harmless and helps if floating.
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus[bus]);
    if (err != ESP_OK) return err;

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = DDC_SLAVE_ADDR,
        .scl_speed_hz    = DDC_I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(s_bus[bus], &dev_cfg, &s_dev[bus]);
}

esp_err_t ddc_init(void)
{
    if (s_inited) return ESP_OK;

    esp_err_t a = init_one_bus(DDC_BUS_A, DDC_BUS_A_SDA_GPIO, DDC_BUS_A_SCL_GPIO, 0);
    if (a != ESP_OK) ESP_LOGE(TAG, "bus A init: %s", esp_err_to_name(a));
    esp_err_t b = init_one_bus(DDC_BUS_B, DDC_BUS_B_SDA_GPIO, DDC_BUS_B_SCL_GPIO, 1);
    if (b != ESP_OK) ESP_LOGE(TAG, "bus B init: %s", esp_err_to_name(b));

    system_status_set("ddc_a", a == ESP_OK ? STATUS_OK : STATUS_ERROR,
                       a == ESP_OK ? "ready" : "init failed: %s", esp_err_to_name(a));
    system_status_set("ddc_b", b == ESP_OK ? STATUS_OK : STATUS_ERROR,
                       b == ESP_OK ? "ready" : "init failed: %s", esp_err_to_name(b));

    s_inited = (a == ESP_OK || b == ESP_OK);  // partial init still useful
    return (a != ESP_OK) ? a : b;
}

esp_err_t ddc_set_vcp(ddc_bus_t bus, uint8_t vcp_code, uint8_t value)
{
    if (bus >= DDC_BUS_COUNT || !s_dev[bus]) return ESP_ERR_INVALID_STATE;

    // Packet: [0x51, 0x84, 0x03, vcp, 0x00, value, checksum]
    // Checksum = 0x6E XOR'd over the 6 payload bytes (0x6E is the dest addr
    // shifted: DDC convention).
    uint8_t pkt[7] = { 0x51, 0x84, 0x03, vcp_code, 0x00, value, 0 };
    uint8_t chk = 0x6E;
    for (int i = 0; i < 6; i++) chk ^= pkt[i];
    pkt[6] = chk;

    esp_err_t err = i2c_master_transmit(s_dev[bus], pkt, sizeof(pkt), 200);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bus %c set vcp 0x%02x=0x%02x: %s",
                 'A' + bus, vcp_code, value, esp_err_to_name(err));
    }
    return err;
}

esp_err_t ddc_get_vcp(ddc_bus_t bus, uint8_t vcp_code, uint8_t *value)
{
    if (bus >= DDC_BUS_COUNT || !s_dev[bus] || !value) return ESP_ERR_INVALID_STATE;

    uint8_t req[5] = { 0x51, 0x82, 0x01, vcp_code, 0 };
    uint8_t chk = 0x6E;
    for (int i = 0; i < 4; i++) chk ^= req[i];
    req[4] = chk;

    esp_err_t err = i2c_master_transmit(s_dev[bus], req, sizeof(req), 200);
    if (err != ESP_OK) return err;

    vTaskDelay(pdMS_TO_TICKS(DDC_GET_REPLY_MS));

    uint8_t resp[11];
    err = i2c_master_receive(s_dev[bus], resp, sizeof(resp), 200);
    if (err != ESP_OK) return err;

    // Reply checksum byte starts from 0x50 (source addr) XOR'd over first 10 bytes.
    uint8_t chk_calc = 0x50;
    for (int i = 0; i < 10; i++) chk_calc ^= resp[i];
    if (chk_calc != resp[10]) {
        ESP_LOGW(TAG, "bus %c get vcp: bad checksum", 'A' + bus);
        return ESP_ERR_INVALID_CRC;
    }
    if (resp[3] != 0x00 || resp[4] != vcp_code) {
        ESP_LOGW(TAG, "bus %c get vcp: error=0x%02x echo=0x%02x",
                 'A' + bus, resp[3], resp[4]);
        return ESP_FAIL;
    }
    *value = resp[9];
    return ESP_OK;
}

static esp_err_t set_with_retry(ddc_bus_t bus, uint8_t vcp, uint8_t value, int retries)
{
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i <= retries; i++) {
        err = ddc_set_vcp(bus, vcp, value);
        if (err == ESP_OK) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(DDC_RETRY_DELAY_MS));
    }
    return err;
}

esp_err_t ddc_switch_to_pc1(void)
{
    ESP_LOGI(TAG, "switch -> PC1");
    esp_err_t a = set_with_retry(DDC_BUS_A, DDC_VCP_INPUT_SOURCE, DDC_U38_INPUT1, 1);
    vTaskDelay(pdMS_TO_TICKS(DDC_RETRY_DELAY_MS));
    esp_err_t b = set_with_retry(DDC_BUS_B, DDC_VCP_INPUT_SOURCE, DDC_U24_INPUT1, 1);
    return (a != ESP_OK) ? a : b;
}

esp_err_t ddc_switch_to_pc2(void)
{
    ESP_LOGI(TAG, "switch -> PC2");
    esp_err_t a = set_with_retry(DDC_BUS_A, DDC_VCP_INPUT_SOURCE, DDC_U38_INPUT2, 1);
    vTaskDelay(pdMS_TO_TICKS(DDC_RETRY_DELAY_MS));
    esp_err_t b = set_with_retry(DDC_BUS_B, DDC_VCP_INPUT_SOURCE, DDC_U24_INPUT2, 1);
    return (a != ESP_OK) ? a : b;
}

bool ddc_monitor_awake(ddc_bus_t bus)
{
    uint8_t v = 0;
    return ddc_get_vcp(bus, DDC_VCP_POWER_MODE, &v) == ESP_OK && v == 0x01;
}
