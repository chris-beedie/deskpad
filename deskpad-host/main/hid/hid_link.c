#include "hid_link.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "system_status.h"

#include <string.h>

static const char *TAG = "hid_link";

// Frame protocol — must match deskpad-hid/src/hid_link.h.
#define FRAME_START   0xAB
#define MAX_PAYLOAD   220

#define MSG_KEYBOARD  0x01
#define MSG_CONSUMER  0x02
#define MSG_SYSTEM    0x03
#define MSG_LOG       0x10
#define MSG_USB_STATUS 0x20
#define MSG_HEARTBEAT 0x21
#define MSG_RESET_REQ 0x30

#define UART_PORT     UART_NUM_1
#define RX_BUF_SIZE   512
#define TX_BUF_SIZE   256

#define LINK_TIMEOUT_US (3 * 1000 * 1000)   // 3 s without heartbeat = link down

static int64_t  s_last_heartbeat_us = 0;
static bool     s_usb_mounted, s_usb_suspended;
static hid_link_usb_status_cb_t s_usb_status_cb;
static void                    *s_usb_status_cb_user;

// ---------------------------------------------------------------------------
// Frame send (callable from any task; UART driver serialises)

static esp_err_t write_frame(uint8_t type, const uint8_t *payload, uint8_t len)
{
    if (len > MAX_PAYLOAD) return ESP_ERR_INVALID_ARG;
    uint8_t xorv = type ^ len;
    for (uint8_t i = 0; i < len; i++) xorv ^= payload[i];

    uint8_t header[3] = { FRAME_START, type, len };
    int n = uart_write_bytes(UART_PORT, (const char *)header, sizeof(header));
    if (n != sizeof(header)) return ESP_FAIL;
    if (len) {
        n = uart_write_bytes(UART_PORT, (const char *)payload, len);
        if (n != len) return ESP_FAIL;
    }
    n = uart_write_bytes(UART_PORT, (const char *)&xorv, 1);
    return (n == 1) ? ESP_OK : ESP_FAIL;
}

esp_err_t hid_link_send_keyboard(uint8_t modifiers, const uint8_t keys[6])
{
    uint8_t pkt[8] = { modifiers, 0, 0, 0, 0, 0, 0, 0 };
    if (keys) memcpy(&pkt[2], keys, 6);
    return write_frame(MSG_KEYBOARD, pkt, sizeof(pkt));
}

esp_err_t hid_link_send_chord_tap(uint8_t modifiers, uint8_t keycode)
{
    const uint8_t down[6] = { keycode, 0, 0, 0, 0, 0 };
    const uint8_t up[6]   = { 0, 0, 0, 0, 0, 0 };
    esp_err_t err = hid_link_send_keyboard(modifiers, down);
    if (err == ESP_OK) err = hid_link_send_keyboard(0, up);
    return err;
}

esp_err_t hid_link_send_consumer_tap(uint16_t usage)
{
    uint8_t down[2] = { (uint8_t)(usage & 0xFF), (uint8_t)(usage >> 8) };
    uint8_t up[2]   = { 0, 0 };
    esp_err_t err = write_frame(MSG_CONSUMER, down, sizeof(down));
    if (err == ESP_OK) err = write_frame(MSG_CONSUMER, up, sizeof(up));
    return err;
}

esp_err_t hid_link_send_system_tap(uint16_t usage)
{
    uint8_t down[2] = { (uint8_t)(usage & 0xFF), (uint8_t)(usage >> 8) };
    uint8_t up[2]   = { 0, 0 };
    esp_err_t err = write_frame(MSG_SYSTEM, down, sizeof(down));
    if (err == ESP_OK) err = write_frame(MSG_SYSTEM, up, sizeof(up));
    return err;
}

// ---------------------------------------------------------------------------
// RX state machine

typedef enum { RX_IDLE, RX_TYPE, RX_LEN, RX_PAYLOAD, RX_CHECKSUM } rx_state_t;

static void handle_frame(uint8_t type, const uint8_t *payload, uint8_t len)
{
    switch (type) {
    case MSG_HEARTBEAT: {
        bool was_up = (s_last_heartbeat_us != 0);
        s_last_heartbeat_us = esp_timer_get_time();
        if (!was_up) system_status_set("hid_link", STATUS_OK, "link up");
        break;
    }
    case MSG_USB_STATUS:
        if (len >= 1) {
            bool was_mounted = s_usb_mounted;
            s_usb_mounted   = !!(payload[0] & 0x01);
            s_usb_suspended = !!(payload[0] & 0x02);
            ESP_LOGI(TAG, "RP2350 USB: mounted=%d suspended=%d",
                     s_usb_mounted, s_usb_suspended);
            if (s_usb_status_cb && (s_usb_mounted != was_mounted)) {
                s_usb_status_cb(s_usb_mounted, s_usb_suspended, s_usb_status_cb_user);
            }
        }
        break;
    case MSG_LOG:
        // null-safe: payload is exactly `len` bytes; the RP2350 doesn't NUL.
        ESP_LOGI(TAG, "RP2350: %.*s", (int)len, (const char *)payload);
        break;
    default:
        ESP_LOGW(TAG, "unknown frame type 0x%02x len %u", type, len);
        break;
    }
}

static void rx_task(void *arg)
{
    (void)arg;
    rx_state_t state = RX_IDLE;
    uint8_t    type  = 0, len = 0, xorv = 0;
    uint16_t   idx   = 0;
    uint8_t    buf[MAX_PAYLOAD];

    for (;;) {
        uint8_t b;
        int n = uart_read_bytes(UART_PORT, &b, 1, portMAX_DELAY);
        if (n != 1) continue;

        switch (state) {
        case RX_IDLE:
            if (b == FRAME_START) { state = RX_TYPE; }
            break;
        case RX_TYPE:
            type = b; xorv = b; state = RX_LEN;
            break;
        case RX_LEN:
            len = b; xorv ^= b; idx = 0;
            state = (len == 0) ? RX_CHECKSUM : RX_PAYLOAD;
            break;
        case RX_PAYLOAD:
            if (idx < MAX_PAYLOAD) buf[idx] = b;
            xorv ^= b;
            idx++;
            if (idx >= len) state = RX_CHECKSUM;
            break;
        case RX_CHECKSUM:
            if (b == xorv) handle_frame(type, buf, len);
            else           ESP_LOGW(TAG, "checksum mismatch (got 0x%02x want 0x%02x)", b, xorv);
            state = RX_IDLE;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Status accessors

bool hid_link_is_up(void)
{
    if (s_last_heartbeat_us == 0) return false;
    return (esp_timer_get_time() - s_last_heartbeat_us) < LINK_TIMEOUT_US;
}

bool hid_link_usb_mounted(void)   { return hid_link_is_up() && s_usb_mounted;   }
bool hid_link_usb_suspended(void) { return hid_link_is_up() && s_usb_suspended; }

void hid_link_set_usb_status_cb(hid_link_usb_status_cb_t cb, void *user)
{
    s_usb_status_cb = cb;
    s_usb_status_cb_user = user;
}

// ---------------------------------------------------------------------------
// Init

esp_err_t hid_link_init(int tx_gpio, int rx_gpio)
{
    if (tx_gpio < 0) tx_gpio = HID_LINK_DEFAULT_TX_GPIO;
    if (rx_gpio < 0) rx_gpio = HID_LINK_DEFAULT_RX_GPIO;

    const uart_config_t cfg = {
        .baud_rate  = HID_LINK_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, RX_BUF_SIZE, TX_BUF_SIZE,
                                        0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, tx_gpio, rx_gpio,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    BaseType_t ok = xTaskCreate(rx_task, "hid_link_rx", 4096, NULL, 5, NULL);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "init tx=GPIO%d rx=GPIO%d baud=%d", tx_gpio, rx_gpio, HID_LINK_BAUD);
    return ESP_OK;
}
