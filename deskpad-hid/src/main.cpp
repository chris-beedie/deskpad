/*********************************************************************
 Deskpad HID bridge — Seeed Studio Xiao RP2350

 Receives HID reports over UART0 from deskpad-host (ESP32-P4-NANO)
 and forwards them to the host PC as a USB composite
 device:
   - HID boot keyboard
   - HID consumer-control (volume/media)
   - USB-CDC (debug log surface for both boards)

 Pin map (Xiao RP2350)
 ─────────────────────────────────────────────────────────────────────
 D6  GP0   UART0 TX -> ESP32 D7 (GPIO44 RX)
 D7  GP1   UART0 RX <- ESP32 D6 (GPIO43 TX)
 GP22      onboard NeoPixel data
 GP23      onboard NeoPixel power enable (drive HIGH)
 USB-C     USB device to PC
 5V/VBUS   Output to ESP32 5V pin (5V@~500mA shared budget)
 ─────────────────────────────────────────────────────────────────────

 Status LED:
   off            : USB unmounted
   solid green    : USB mounted, link up
   green pulse    : keyboard report just forwarded
   solid red      : link down (no heartbeat for >3s)
   slow blue      : USB suspended
*********************************************************************/

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Adafruit_NeoPixel.h>
#include "hid_link.h"

// ── Pins ─────────────────────────────────────────────────────────────────────
constexpr uint8_t LINK_TX_PIN      = 0;   // D6 -> ESP32 RX
constexpr uint8_t LINK_RX_PIN      = 1;   // D7 <- ESP32 TX
constexpr uint8_t ESP32_RST_PIN    = 7;   // D5 -> ESP32 RST pad (back side of
                                          //   Xiao ESP32-S3, near USB-C). Drive
                                          //   low briefly to hardware-reset
                                          //   the ESP32. Held high-Z when not
                                          //   resetting (ESP32 RST has its own
                                          //   pull-up).
constexpr uint8_t NEOPIXEL_PIN     = 22;
constexpr uint8_t NEOPIXEL_POWER   = 23;

// ── Timing ───────────────────────────────────────────────────────────────────
constexpr uint32_t LINK_TIMEOUT_MS    = 3000;
constexpr uint32_t LED_PULSE_MS       = 60;
constexpr uint32_t SUSPEND_PULSE_MS   = 1500;
constexpr uint32_t USB_REPUBLISH_MS   = 1000;   // resend USB_STATUS once a second

// ── HID descriptor: keyboard + consumer-control + system-control ────────────
enum {
    REPORT_ID_KEYBOARD = 1,
    REPORT_ID_CONSUMER = 2,
    REPORT_ID_SYSTEM   = 3,
};

// System Control collection (Generic Desktop usage 0x80). TinyUSB ships an
// array-style descriptor: a 1-byte report carries a 2-bit index in bits 0-1,
// with values 1=Power Down, 2=Sleep, 3=Wake Up, 0=release.
uint8_t const desc_hid_report[] = {
    TUD_HID_REPORT_DESC_KEYBOARD      (HID_REPORT_ID(REPORT_ID_KEYBOARD)),
    TUD_HID_REPORT_DESC_CONSUMER      (HID_REPORT_ID(REPORT_ID_CONSUMER)),
    TUD_HID_REPORT_DESC_SYSTEM_CONTROL(HID_REPORT_ID(REPORT_ID_SYSTEM)),
};

Adafruit_USBD_HID usb_hid;

// ── Status LED ───────────────────────────────────────────────────────────────
Adafruit_NeoPixel pixel(1, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

// ── Link state ───────────────────────────────────────────────────────────────
static uint32_t last_link_rx_ms = 0;
static uint32_t led_pulse_until = 0;

// ── HID state (for press/release diff on keyboard reports) ──────────────────
static uint8_t  prev_modifiers = 0;
static uint8_t  prev_keys[6]   = {0};
static uint16_t prev_consumer  = 0;

// ── Forward declarations ─────────────────────────────────────────────────────
static void onKeyboard(const uint8_t kb[8]);
static void onConsumer(uint16_t usage_code);
static void onSystem  (uint16_t usage_code);
static void onReset   (void);
static void onLog     (const char* msg);
static void updateStatusLED();
static void publishUsbStatus();

// ── Setup / loop ─────────────────────────────────────────────────────────────
void setup() {
    // Onboard NeoPixel power rail must be enabled before driving the LED.
    pinMode(NEOPIXEL_POWER, OUTPUT);
    digitalWrite(NEOPIXEL_POWER, HIGH);
    pixel.begin();
    pixel.setBrightness(40);
    pixel.setPixelColor(0, pixel.Color(0, 0, 30));   // boot: dim blue
    pixel.show();

    // Composite USB: HID + CDC. CDC is auto-included by TinyUSB when
    // CFG_TUD_CDC=1; Serial maps to it in arduino-pico.
    usb_hid.setReportDescriptor(desc_hid_report, sizeof(desc_hid_report));
    usb_hid.setStringDescriptor("Deskpad HID");
    usb_hid.begin();

    Serial.begin(115200);   // USB-CDC — appears as a serial port to the PC

    // Default the ESP32 reset pin to high-impedance — the ESP32's RST has
    // its own pull-up, so floating = held high = not reset. We only flip
    // it to OUTPUT-LOW briefly when we actually want to reset the ESP32.
    pinMode(ESP32_RST_PIN, INPUT);

    hidLinkBegin(LINK_TX_PIN, LINK_RX_PIN);
    hidLinkOnKeyboard(onKeyboard);
    hidLinkOnConsumer(onConsumer);
    hidLinkOnSystem  (onSystem);
    hidLinkOnReset   (onReset);
    hidLinkOnLog     (onLog);

    // Wait briefly for USB enumeration before we start chattering.
    uint32_t start = millis();
    while (!TinyUSBDevice.mounted() && (millis() - start) < 2000) {
        delay(10);
    }
    publishUsbStatus();

    Serial.println("[HID] deskpad-hid (Xiao RP2350) ready");
}

void loop() {
    static bool     prev_mounted    = false;
    static bool     prev_suspended  = false;
    static uint32_t last_republish  = 0;

    bool mounted   = TinyUSBDevice.mounted();
    bool suspended = TinyUSBDevice.suspended();
    uint32_t now   = millis();

    // Publish on every transition AND once a second so the ESP32 always sees
    // a fresh value, even if it just rebooted (e.g. after OTA) and lost its
    // in-memory copy of our state. Only the transition path logs to Serial
    // so the periodic resend doesn't spam the CDC console.
    if (mounted != prev_mounted || suspended != prev_suspended) {
        prev_mounted   = mounted;
        prev_suspended = suspended;
        last_republish = now;
        publishUsbStatus();
    } else if (now - last_republish >= USB_REPUBLISH_MS) {
        last_republish = now;
        hidLinkSendUsbStatus(mounted, suspended);
    }

    hidLinkLoop();
    updateStatusLED();
}

// ── Link callbacks ──────────────────────────────────────────────────────────
//
// Keyboard report (8 bytes, boot protocol):
//   buf[0]   = modifiers
//   buf[1]   = reserved
//   buf[2-7] = keycodes (up to 6 simultaneously pressed)
//
// We send it as a TinyUSB keyboard report. TinyUSB's keyboard report
// descriptor uses the same layout, so we can pass it through directly
// after waiting for the previous report to complete.
static void onKeyboard(const uint8_t kb[8]) {
    last_link_rx_ms = millis();
    led_pulse_until = millis() + LED_PULSE_MS;

    // Wait briefly for the previous report to be picked up.
    uint32_t start = millis();
    while (!usb_hid.ready() && (millis() - start) < 5) delay(0);
    if (!usb_hid.ready()) return;

    uint8_t modifier = kb[0];
    uint8_t keycodes[6] = { kb[2], kb[3], kb[4], kb[5], kb[6], kb[7] };
    usb_hid.keyboardReport(REPORT_ID_KEYBOARD, modifier, keycodes);

    // Stash for diagnostics — TinyUSB does the actual diffing on the wire.
    prev_modifiers = modifier;
    memcpy(prev_keys, keycodes, 6);
}

// Consumer-control: 16-bit usage code (0 = release).
static void onConsumer(uint16_t usage_code) {
    last_link_rx_ms = millis();
    led_pulse_until = millis() + LED_PULSE_MS;

    uint32_t start = millis();
    while (!usb_hid.ready() && (millis() - start) < 5) delay(0);
    if (!usb_hid.ready()) return;

    usb_hid.sendReport16(REPORT_ID_CONSUMER, usage_code);
    prev_consumer = usage_code;
}

// System-control: usage codes 0x81/0x82/0x83, 0 = release. Translated to
// the 1-byte bit-field report defined in TUD_HID_REPORT_DESC_SYSTEM_CONTROL.
static void onSystem(uint16_t usage_code) {
    last_link_rx_ms = millis();
    led_pulse_until = millis() + LED_PULSE_MS;

    // TinyUSB descriptor uses array index encoding: 1=Power Down, 2=Sleep,
    // 3=Wake Up, 0=release.
    uint8_t report = 0;
    switch (usage_code) {
        case 0x81: report = 1; break;
        case 0x82: report = 2; break;
        case 0x83: report = 3; break;
        default:   report = 0; break;
    }

    uint32_t start = millis();
    while (!usb_hid.ready() && (millis() - start) < 5) delay(0);
    if (!usb_hid.ready()) return;

    usb_hid.sendReport(REPORT_ID_SYSTEM, &report, 1);
}

// ESP32 asked us to hardware-reset it. Drive D5 low for 20ms (ESP32-S3 needs
// only ~1 us on RST/EN to trigger reset; 20 ms is generous), then release.
// If the wire mod isn't installed this is a no-op — the unwired GPIO has no
// effect on the ESP32 — and the ESP32 will fall through to its software reset
// fallback after a short timeout.
static void onReset(void) {
    last_link_rx_ms = millis();
    Serial.println("[RST] reset request from ESP32 — driving RST low");
    pinMode(ESP32_RST_PIN, OUTPUT);
    digitalWrite(ESP32_RST_PIN, LOW);
    delay(20);
    pinMode(ESP32_RST_PIN, INPUT);   // back to high-Z
}

// Log line from the ESP32 — print to USB-CDC with a tag.
static void onLog(const char* msg) {
    last_link_rx_ms = millis();
    Serial.print("[ESP] ");
    Serial.println(msg);
}

// ── Status LED ──────────────────────────────────────────────────────────────
static void updateStatusLED() {
    uint32_t now = millis();
    bool mounted   = TinyUSBDevice.mounted();
    bool suspended = TinyUSBDevice.suspended();
    bool link_up   = (now - last_link_rx_ms) < LINK_TIMEOUT_MS && last_link_rx_ms != 0;

    uint32_t colour;
    if (!mounted) {
        colour = 0;                                  // off
    } else if (suspended) {
        // slow blue breathing during suspend
        uint32_t phase = (now % SUSPEND_PULSE_MS);
        uint8_t  v = (phase < SUSPEND_PULSE_MS / 2)
                       ? (phase * 60 / (SUSPEND_PULSE_MS / 2))
                       : ((SUSPEND_PULSE_MS - phase) * 60 / (SUSPEND_PULSE_MS / 2));
        colour = pixel.Color(0, 0, v);
    } else if (!link_up) {
        colour = pixel.Color(60, 0, 0);              // red — link down
    } else if (now < led_pulse_until) {
        colour = pixel.Color(0, 80, 0);              // green pulse on report
    } else {
        colour = pixel.Color(0, 25, 0);              // dim green idle
    }
    pixel.setPixelColor(0, colour);
    pixel.show();
}

// Tell the ESP32 about our USB enumeration state so its Web UI/MQTT can
// surface "PC sees keyboard: yes/no".
static void publishUsbStatus() {
    bool mounted   = TinyUSBDevice.mounted();
    bool suspended = TinyUSBDevice.suspended();
    hidLinkSendUsbStatus(mounted, suspended);
    Serial.printf("[USB] mounted=%d suspended=%d\n", mounted, suspended);
}
