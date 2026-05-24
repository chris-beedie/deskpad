/*********************************************************************
 hid_link — UART link from deskpad-host (ESP32-P4-NANO)

 Receives:  keyboard reports, consumer-control reports, debug log lines.
 Sends:     USB-to-PC status, 1Hz heartbeat.

 Frame: 0xAB | type | len | payload[len] | xor(type, len, payload)
*********************************************************************/

#include "hid_link.h"

// arduino-pico exposes the RP2350's UART0 via SerialPIO/Serial1.
// On the Xiao RP2350: D6 = GP0 = UART0 TX, D7 = GP1 = UART0 RX.
// Serial1 maps to UART0 by default in arduino-pico.
#define LinkUart Serial1

static constexpr uint8_t  FRAME_START          = 0xAB;
static constexpr uint16_t MAX_PAYLOAD          = 220;
static constexpr uint32_t HEARTBEAT_INTERVAL_MS = 1000;

// ── RX state machine ─────────────────────────────────────────────────────────
enum RxState : uint8_t { RX_IDLE, RX_TYPE, RX_LEN, RX_PAYLOAD, RX_CHECKSUM };
static RxState  rx_state = RX_IDLE;
static uint8_t  rx_type, rx_len, rx_xor;
static uint16_t rx_idx;
static uint8_t  rx_buf[MAX_PAYLOAD + 1];

// ── Callbacks ────────────────────────────────────────────────────────────────
static LinkKeyboardCb cb_keyboard = nullptr;
static LinkConsumerCb cb_consumer = nullptr;
static LinkSystemCb   cb_system   = nullptr;
static LinkResetCb    cb_reset    = nullptr;
static LinkLogCb      cb_log      = nullptr;

// ── Heartbeat state ──────────────────────────────────────────────────────────
static uint32_t last_hb_sent = 0;

// ── Helpers ──────────────────────────────────────────────────────────────────
static void writeFrame(uint8_t type, const uint8_t* payload, uint8_t len) {
    if (len > MAX_PAYLOAD) return;
    uint8_t xorv = type ^ len;
    for (uint8_t i = 0; i < len; i++) xorv ^= payload[i];
    uint8_t header[3] = { FRAME_START, type, len };
    LinkUart.write(header, 3);
    if (len) LinkUart.write(payload, len);
    LinkUart.write(&xorv, 1);
}

static void handleFrame(uint8_t type, uint8_t* payload, uint8_t len) {
    switch (type) {
    case LinkMsg::KEYBOARD:
        if (len >= 8 && cb_keyboard) cb_keyboard(payload);
        break;
    case LinkMsg::CONSUMER:
        if (len >= 2 && cb_consumer) {
            uint16_t usage = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
            cb_consumer(usage);
        }
        break;
    case LinkMsg::SYSTEM:
        if (len >= 2 && cb_system) {
            uint16_t usage = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
            cb_system(usage);
        }
        break;
    case LinkMsg::RESET_REQ:
        if (cb_reset) cb_reset();
        break;
    case LinkMsg::LOG:
        if (cb_log) {
            payload[len] = 0;  // safe: rx_buf is sized MAX_PAYLOAD+1
            cb_log((const char*)payload);
        }
        break;
    default:
        break;
    }
}

static void rxFeed(uint8_t b) {
    switch (rx_state) {
    case RX_IDLE:
        if (b == FRAME_START) rx_state = RX_TYPE;
        break;
    case RX_TYPE:
        rx_type  = b;
        rx_xor   = b;
        rx_state = RX_LEN;
        break;
    case RX_LEN:
        rx_len   = b;
        rx_xor  ^= b;
        rx_idx   = 0;
        if (rx_len > MAX_PAYLOAD) { rx_state = RX_IDLE; break; }
        rx_state = (rx_len == 0) ? RX_CHECKSUM : RX_PAYLOAD;
        break;
    case RX_PAYLOAD:
        rx_buf[rx_idx++] = b;
        rx_xor ^= b;
        if (rx_idx >= rx_len) rx_state = RX_CHECKSUM;
        break;
    case RX_CHECKSUM:
        if (b == rx_xor) handleFrame(rx_type, rx_buf, rx_len);
        rx_state = RX_IDLE;
        break;
    }
}

// ── Public API ───────────────────────────────────────────────────────────────
void hidLinkBegin(uint8_t tx_pin, uint8_t rx_pin, uint32_t baud) {
    LinkUart.setTX(tx_pin);
    LinkUart.setRX(rx_pin);
    LinkUart.begin(baud);
}

void hidLinkLoop() {
    while (LinkUart.available()) rxFeed((uint8_t)LinkUart.read());

    uint32_t now = millis();
    if (now - last_hb_sent >= HEARTBEAT_INTERVAL_MS) {
        last_hb_sent = now;
        writeFrame(LinkMsg::HEARTBEAT, nullptr, 0);
    }
}

void hidLinkOnKeyboard(LinkKeyboardCb cb) { cb_keyboard = cb; }
void hidLinkOnConsumer(LinkConsumerCb cb) { cb_consumer = cb; }
void hidLinkOnSystem  (LinkSystemCb   cb) { cb_system   = cb; }
void hidLinkOnReset   (LinkResetCb    cb) { cb_reset    = cb; }
void hidLinkOnLog     (LinkLogCb      cb) { cb_log      = cb; }

void hidLinkSendUsbStatus(bool mounted, bool suspended) {
    uint8_t pl = (mounted ? 0x01 : 0) | (suspended ? 0x02 : 0);
    writeFrame(LinkMsg::USB_STATUS, &pl, 1);
}
