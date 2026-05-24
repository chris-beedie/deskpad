#pragma once

#include "akp03e.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "usb/usb_host.h"

// Hardware identifiers, confirmed from pcap (VID/PID) and mirajazz repo.
#define AKP03E_VID            0x0300
#define AKP03E_REV2_PID       0x3002

// Endpoint addresses on interface 0.
#define AKP03E_EP_OUT         0x03
#define AKP03E_EP_IN          0x82

// Wire framing. OUT endpoint MPS = 1024 (image chunks / commands).
// IN endpoint MPS = 512 (one input report per URB, see endpoint descriptor in
// captures/akp_capture.pcap).
#define AKP03E_PACKET_SIZE    1024   // OUT payload per URB
#define AKP03E_IN_PACKET_SIZE 512    // IN payload per URB
#define AKP03E_OUT_MAGIC_0    0x43   // 'C'
#define AKP03E_OUT_MAGIC_1    0x52   // 'R'
#define AKP03E_OUT_MAGIC_2    0x54   // 'T'
#define AKP03E_IN_MAGIC_0     0x41   // 'A'
#define AKP03E_IN_MAGIC_1     0x43   // 'C'
#define AKP03E_IN_MAGIC_2     0x4B   // 'K'

// Shared state between akp03e.c and akp03e_usb.c.
typedef struct {
    usb_host_client_handle_t client;
    usb_device_handle_t      dev;       // NULL when no device

    SemaphoreHandle_t        lock;      // guards dev + out_xfer access
    usb_transfer_t          *out_xfer;
    usb_transfer_t          *in_xfer;
    SemaphoreHandle_t        out_done;  // posted from out callback

    akp03e_event_cb_t        cb;
    void                    *cb_user;

    bool                     initialized;
    bool                     attached;
} akp03e_ctx_t;

extern akp03e_ctx_t g_akp;

// USB layer entry points.
esp_err_t akp03e_usb_start(void);
esp_err_t akp03e_send_out(const uint8_t *payload, size_t len);
