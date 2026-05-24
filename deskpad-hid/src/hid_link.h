#pragma once
#include <Arduino.h>

// ── Wire protocol shared with deskpad-host (ESP32-P4-NANO) ──────────────────
// Frame layout on the UART:
//   [0xAB][type][len][payload 0..len-1][xor checksum of type|len|payload]
// Baud: 1 Mbps, 8N1.
namespace LinkMsg {
    constexpr uint8_t KEYBOARD       = 0x01;  // 8-byte boot keyboard report
    constexpr uint8_t CONSUMER       = 0x02;  // 2-byte consumer-control report (LE)
    constexpr uint8_t SYSTEM         = 0x03;  // 2-byte system-control usage code (LE)
                                              //   0x81 = System Power Down
                                              //   0x82 = System Sleep
                                              //   0x83 = System Wake Up
                                              //   0x00 = release
    constexpr uint8_t LOG            = 0x10;  // ESP32 -> RP2350 debug log line
    constexpr uint8_t USB_STATUS     = 0x20;  // RP2350 -> ESP32 USB-to-PC state
    constexpr uint8_t HEARTBEAT      = 0x21;  // RP2350 -> ESP32 1Hz keepalive
    constexpr uint8_t RESET_REQ      = 0x30;  // ESP32 -> RP2350: drive the
                                              //   wired ESP32 RST pin low
}

// Callbacks invoked from hidLinkLoop() on receipt of each message type.
//   kb        - 8-byte boot keyboard report
//   consumer  - little-endian 16-bit usage code
//   log       - NUL-terminated UTF-8 string (lifetime: this call only)
typedef void (*LinkKeyboardCb)(const uint8_t kb[8]);
typedef void (*LinkConsumerCb)(uint16_t usage_code);
typedef void (*LinkSystemCb)  (uint16_t usage_code);
typedef void (*LinkResetCb)   (void);
typedef void (*LinkLogCb)(const char* msg);

// Initialise UART0 link from the ESP32 on the given pins (GP0/GP1 by default).
void hidLinkBegin(uint8_t tx_pin, uint8_t rx_pin, uint32_t baud = 1000000);

// Pump receive buffer + emit periodic heartbeat. Call every loop().
void hidLinkLoop();

// Register callbacks. Pass nullptr to ignore.
void hidLinkOnKeyboard(LinkKeyboardCb cb);
void hidLinkOnConsumer(LinkConsumerCb cb);
void hidLinkOnSystem  (LinkSystemCb   cb);
void hidLinkOnReset   (LinkResetCb    cb);
void hidLinkOnLog     (LinkLogCb cb);

// Send our USB-to-PC mount status to the ESP32 (call when it changes).
//   mounted   - host has enumerated us
//   suspended - USB is in suspend
void hidLinkSendUsbStatus(bool mounted, bool suspended);
