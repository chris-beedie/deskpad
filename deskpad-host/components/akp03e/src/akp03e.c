#include "akp03e_priv.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "akp03e";

esp_err_t akp03e_init(akp03e_event_cb_t cb, void *user)
{
    if (g_akp.initialized) return ESP_ERR_INVALID_STATE;
    g_akp.cb = cb;
    g_akp.cb_user = user;
    g_akp.initialized = true;
    return akp03e_usb_start();
}

// ---- Outgoing commands ----------------------------------------------------

static esp_err_t send_init_sequence(void)
{
    // CRT + DIS: tell the device to initialize displays.
    const uint8_t dis[] = { AKP03E_OUT_MAGIC_0, AKP03E_OUT_MAGIC_1, AKP03E_OUT_MAGIC_2,
                            0x00, 0x00, 'D', 'I', 'S' };
    ESP_RETURN_ON_ERROR(akp03e_send_out(dis, sizeof(dis)), TAG, "DIS");

    // CRT + LIG 0: brightness 0, settled state before a real value is set.
    const uint8_t lig0[] = { AKP03E_OUT_MAGIC_0, AKP03E_OUT_MAGIC_1, AKP03E_OUT_MAGIC_2,
                             0x00, 0x00, 'L', 'I', 'G', 0x00, 0x00, 0x00 };
    return akp03e_send_out(lig0, sizeof(lig0));
}

static bool ensure_initialized_once(void)
{
    static bool done = false;
    if (done) return true;
    if (send_init_sequence() == ESP_OK) {
        done = true;
        return true;
    }
    return false;
}

esp_err_t akp03e_set_brightness(uint8_t percent)
{
    if (!g_akp.attached) return ESP_ERR_INVALID_STATE;
    if (percent > 100) percent = 100;
    ensure_initialized_once();

    const uint8_t lig[] = { AKP03E_OUT_MAGIC_0, AKP03E_OUT_MAGIC_1, AKP03E_OUT_MAGIC_2,
                            0x00, 0x00, 'L', 'I', 'G', 0x00, 0x00, percent };
    return akp03e_send_out(lig, sizeof(lig));
}

esp_err_t akp03e_clear_key(uint8_t lcd_key)
{
    if (!g_akp.attached) return ESP_ERR_INVALID_STATE;
    if (lcd_key >= AKP03E_LCD_KEY_COUNT) return ESP_ERR_INVALID_ARG;
    ensure_initialized_once();

    const uint8_t cle[] = { AKP03E_OUT_MAGIC_0, AKP03E_OUT_MAGIC_1, AKP03E_OUT_MAGIC_2,
                            0x00, 0x00, 'C', 'L', 'E',
                            0x00, 0x00, 0x00, (uint8_t)(lcd_key + 1) };
    ESP_RETURN_ON_ERROR(akp03e_send_out(cle, sizeof(cle)), TAG, "CLE");

    const uint8_t stp[] = { AKP03E_OUT_MAGIC_0, AKP03E_OUT_MAGIC_1, AKP03E_OUT_MAGIC_2,
                            0x00, 0x00, 'S', 'T', 'P' };
    return akp03e_send_out(stp, sizeof(stp));
}

esp_err_t akp03e_clear_all_keys(void)
{
    if (!g_akp.attached) return ESP_ERR_INVALID_STATE;
    ensure_initialized_once();

    const uint8_t cle[] = { AKP03E_OUT_MAGIC_0, AKP03E_OUT_MAGIC_1, AKP03E_OUT_MAGIC_2,
                            0x00, 0x00, 'C', 'L', 'E',
                            0x00, 0x00, 0x00, 0xFF };
    ESP_RETURN_ON_ERROR(akp03e_send_out(cle, sizeof(cle)), TAG, "CLE all");

    const uint8_t stp[] = { AKP03E_OUT_MAGIC_0, AKP03E_OUT_MAGIC_1, AKP03E_OUT_MAGIC_2,
                            0x00, 0x00, 'S', 'T', 'P' };
    return akp03e_send_out(stp, sizeof(stp));
}

esp_err_t akp03e_set_key_jpeg(uint8_t lcd_key, const uint8_t *jpeg, size_t len)
{
    if (!g_akp.attached) return ESP_ERR_INVALID_STATE;
    if (lcd_key >= AKP03E_LCD_KEY_COUNT) return ESP_ERR_INVALID_ARG;
    if (!jpeg || len == 0 || len > 0xFFFF) return ESP_ERR_INVALID_ARG;
    ensure_initialized_once();

    // BAT header: announce image length (big-endian u16) and key (1-indexed).
    const uint8_t bat[] = {
        AKP03E_OUT_MAGIC_0, AKP03E_OUT_MAGIC_1, AKP03E_OUT_MAGIC_2, 0x00, 0x00,
        'B', 'A', 'T', 0x00, 0x00,
        (uint8_t)(len >> 8), (uint8_t)(len & 0xFF), (uint8_t)(lcd_key + 1),
    };
    ESP_RETURN_ON_ERROR(akp03e_send_out(bat, sizeof(bat)), TAG, "BAT");

    // Stream raw JPEG bytes in PACKET_SIZE chunks; each chunk is zero-padded
    // inside akp03e_send_out().
    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > AKP03E_PACKET_SIZE) chunk = AKP03E_PACKET_SIZE;
        ESP_RETURN_ON_ERROR(akp03e_send_out(jpeg + off, chunk), TAG, "img chunk");
        off += chunk;
    }

    const uint8_t stp[] = { AKP03E_OUT_MAGIC_0, AKP03E_OUT_MAGIC_1, AKP03E_OUT_MAGIC_2,
                            0x00, 0x00, 'S', 'T', 'P' };
    return akp03e_send_out(stp, sizeof(stp));
}

esp_err_t akp03e_set_boot_logo(const uint8_t *jpeg, size_t len)
{
    if (!g_akp.attached) return ESP_ERR_INVALID_STATE;
    if (!jpeg || len == 0 || len > 0xFFFF) return ESP_ERR_INVALID_ARG;
    ensure_initialized_once();

    // LOG announce: "LOG" + 0x00 0x00 + big-endian u16 length.
    const uint8_t log_pkt[] = {
        AKP03E_OUT_MAGIC_0, AKP03E_OUT_MAGIC_1, AKP03E_OUT_MAGIC_2,
        0x00, 0x00, 'L', 'O', 'G',
        0x00, 0x00,
        (uint8_t)(len >> 8), (uint8_t)(len & 0xFF),
    };
    ESP_RETURN_ON_ERROR(akp03e_send_out(log_pkt, sizeof(log_pkt)), TAG, "LOG announce");

    // Flush before streaming the image data — opposite of BAT/key-image
    // ordering. (See ajazz-sdk set_logo_image.)
    const uint8_t stp[] = {
        AKP03E_OUT_MAGIC_0, AKP03E_OUT_MAGIC_1, AKP03E_OUT_MAGIC_2,
        0x00, 0x00, 'S', 'T', 'P',
    };
    ESP_RETURN_ON_ERROR(akp03e_send_out(stp, sizeof(stp)), TAG, "STP flush");

    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > AKP03E_PACKET_SIZE) chunk = AKP03E_PACKET_SIZE;
        ESP_RETURN_ON_ERROR(akp03e_send_out(jpeg + off, chunk), TAG, "logo chunk");
        off += chunk;
    }
    return ESP_OK;
}

esp_err_t akp03e_sleep(void)
{
    if (!g_akp.attached) return ESP_ERR_INVALID_STATE;
    const uint8_t han[] = { AKP03E_OUT_MAGIC_0, AKP03E_OUT_MAGIC_1, AKP03E_OUT_MAGIC_2,
                            0x00, 0x00, 'H', 'A', 'N' };
    return akp03e_send_out(han, sizeof(han));
}

esp_err_t akp03e_shutdown(void)
{
    if (!g_akp.attached) return ESP_ERR_INVALID_STATE;
    const uint8_t cle_dc[] = { AKP03E_OUT_MAGIC_0, AKP03E_OUT_MAGIC_1, AKP03E_OUT_MAGIC_2,
                               0x00, 0x00, 'C', 'L', 'E',
                               0x00, 0x00, 'D', 'C' };
    akp03e_send_out(cle_dc, sizeof(cle_dc));

    const uint8_t han[] = { AKP03E_OUT_MAGIC_0, AKP03E_OUT_MAGIC_1, AKP03E_OUT_MAGIC_2,
                            0x00, 0x00, 'H', 'A', 'N' };
    return akp03e_send_out(han, sizeof(han));
}

// ---- Incoming event parsing ----------------------------------------------

static void emit(const akp03e_event_t *ev)
{
    if (g_akp.cb) g_akp.cb(ev, g_akp.cb_user);
}

// Called from akp03e_usb.c when an IN report arrives.
// Layout (rev 2 / protocol v3, verified against captures/button.pcap):
//   [0..2] = "ACK"
//   [3..4] = 0x00 0x00
//   [5..6] = "OK"      (some firmwares; some send other 2-byte tags)
//   [7..8] = 0x00 0x00
//   [9]    = input code
//   [10]   = state (1=down, 0=up)
void akp03e_handle_input(const uint8_t *data, size_t len)
{
    if (len < 11) return;
    if (data[0] != AKP03E_IN_MAGIC_0 ||
        data[1] != AKP03E_IN_MAGIC_1 ||
        data[2] != AKP03E_IN_MAGIC_2) {
        return;
    }
    const uint8_t code  = data[9];
    const uint8_t state = data[10];
    ESP_LOGD(TAG, "raw in: code=0x%02x state=0x%02x", code, state);

    akp03e_event_t ev = {0};

    // LCD buttons 1..6 -> index 0..5
    if (code >= 1 && code <= 6) {
        ev.type = AKP03E_EVT_BUTTON;
        ev.index = code - 1;
        ev.pressed = (state != 0);
        emit(&ev);
        return;
    }
    // Non-LCD buttons
    switch (code) {
    case 0x25: ev.type = AKP03E_EVT_BUTTON; ev.index = 6; ev.pressed = state != 0; emit(&ev); return;
    case 0x30: ev.type = AKP03E_EVT_BUTTON; ev.index = 7; ev.pressed = state != 0; emit(&ev); return;
    case 0x31: ev.type = AKP03E_EVT_BUTTON; ev.index = 8; ev.pressed = state != 0; emit(&ev); return;
    // Encoder presses (left=0x33, top=0x35, right=0x34) — order from opendeck-akp03/inputs.rs
    case 0x33: ev.type = AKP03E_EVT_ENCODER_PRESS; ev.index = 0; ev.pressed = state != 0; emit(&ev); return;
    case 0x35: ev.type = AKP03E_EVT_ENCODER_PRESS; ev.index = 1; ev.pressed = state != 0; emit(&ev); return;
    case 0x34: ev.type = AKP03E_EVT_ENCODER_PRESS; ev.index = 2; ev.pressed = state != 0; emit(&ev); return;
    // Encoder twists
    case 0x90: ev.type = AKP03E_EVT_ENCODER_TWIST; ev.index = 0; ev.twist = -1; emit(&ev); return;
    case 0x91: ev.type = AKP03E_EVT_ENCODER_TWIST; ev.index = 0; ev.twist = +1; emit(&ev); return;
    case 0x50: ev.type = AKP03E_EVT_ENCODER_TWIST; ev.index = 1; ev.twist = -1; emit(&ev); return;
    case 0x51: ev.type = AKP03E_EVT_ENCODER_TWIST; ev.index = 1; ev.twist = +1; emit(&ev); return;
    case 0x60: ev.type = AKP03E_EVT_ENCODER_TWIST; ev.index = 2; ev.twist = -1; emit(&ev); return;
    case 0x61: ev.type = AKP03E_EVT_ENCODER_TWIST; ev.index = 2; ev.twist = +1; emit(&ev); return;
    case 0x00:
        // "all released" idle report — nothing actionable
        return;
    default:
        ESP_LOGD(TAG, "unhandled input code 0x%02x state 0x%02x", code, state);
    }
}
