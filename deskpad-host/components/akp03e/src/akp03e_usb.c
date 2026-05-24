#include "akp03e_priv.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

#include <string.h>

static const char *TAG = "akp03e_usb";

akp03e_ctx_t g_akp = {0};

static void parse_input_and_fire(const uint8_t *data, size_t len);

static void in_xfer_cb(usb_transfer_t *t)
{
    if (t->status == USB_TRANSFER_STATUS_COMPLETED && t->actual_num_bytes > 0) {
        parse_input_and_fire(t->data_buffer, t->actual_num_bytes);
    } else if (t->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGD(TAG, "in xfer status=%d", t->status);
    }
    // Re-arm if the device is still attached.
    if (g_akp.attached) {
        esp_err_t err = usb_host_transfer_submit(t);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "resubmit in xfer failed: %s", esp_err_to_name(err));
        }
    }
}

static void out_xfer_cb(usb_transfer_t *t)
{
    ESP_LOGD(TAG, "out xfer cb status=%d actual=%d", t->status, t->actual_num_bytes);
    if (t->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGW(TAG, "out xfer status=%d (req=%d, actual=%d)",
                 t->status, t->num_bytes, t->actual_num_bytes);
    }
    xSemaphoreGive(g_akp.out_done);
}

esp_err_t akp03e_send_out(const uint8_t *payload, size_t len)
{
    if (!g_akp.attached || !g_akp.out_xfer) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len > AKP03E_PACKET_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    xSemaphoreTake(g_akp.lock, portMAX_DELAY);

    memcpy(g_akp.out_xfer->data_buffer, payload, len);
    if (len < AKP03E_PACKET_SIZE) {
        memset(g_akp.out_xfer->data_buffer + len, 0, AKP03E_PACKET_SIZE - len);
    }
    g_akp.out_xfer->num_bytes = AKP03E_PACKET_SIZE;
    g_akp.out_xfer->bEndpointAddress = AKP03E_EP_OUT;
    g_akp.out_xfer->device_handle = g_akp.dev;
    g_akp.out_xfer->callback = out_xfer_cb;
    g_akp.out_xfer->context = NULL;

    esp_err_t err = usb_host_transfer_submit(g_akp.out_xfer);
    if (err != ESP_OK) {
        xSemaphoreGive(g_akp.lock);
        return err;
    }
    // Wait for completion (USB HS @ 1024 bytes completes in microseconds, but
    // give the device ample time to ACK).
    if (xSemaphoreTake(g_akp.out_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "out xfer timeout (no callback in 2s)");
        err = ESP_ERR_TIMEOUT;
    }
    xSemaphoreGive(g_akp.lock);
    return err;
}

// Sync control transfer helper. cb_dummy is used to signal completion.
static SemaphoreHandle_t s_ctrl_done;
static void ctrl_xfer_cb(usb_transfer_t *t)
{
    ESP_LOGD(TAG, "ctrl xfer cb status=%d actual=%d", t->status, t->actual_num_bytes);
    xSemaphoreGive(s_ctrl_done);
}

// Submit a control transfer, wait for completion. Returns ESP_OK on success.
// `data_len` is the wLength; the payload (if any) lives at `transfer->data_buffer + 8`
// for OUT, or arrives there from the device for IN. Setup packet must already be filled.
static esp_err_t ctrl_submit_wait(usb_transfer_t *xfer)
{
    xfer->callback = ctrl_xfer_cb;
    xfer->context = NULL;
    esp_err_t err = usb_host_transfer_submit_control(g_akp.client, xfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ctrl submit: %s", esp_err_to_name(err));
        return err;
    }
    if (xSemaphoreTake(s_ctrl_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "ctrl xfer timeout");
        return ESP_ERR_TIMEOUT;
    }
    if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGW(TAG, "ctrl xfer status=%d", xfer->status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Mirror Windows' HID handshake captured in captures/akp_capture.pcap:
//   SET_IDLE(if=0), SET_IDLE(if=1), SET_REPORT(if=1, OUT report 0, [0]), GET_REPORT(if=0, IN report 0, 512).
// The trailing GET_REPORT appears to be what unblocks the vendor interrupt-OUT pipe.
static esp_err_t do_hid_init_handshake(void)
{
    usb_transfer_t *xfer = NULL;
    esp_err_t err = usb_host_transfer_alloc(8 + 512, 0, &xfer);
    if (err != ESP_OK) return err;
    xfer->device_handle = g_akp.dev;
    xfer->bEndpointAddress = 0;  // EP0

    usb_setup_packet_t *setup = (usb_setup_packet_t *)xfer->data_buffer;

    // For control transfers, bEndpointAddress carries only the direction bit.
    // 0x00 for OUT (bmRequestType bit7=0), 0x80 for IN (bit7=1).
    // num_bytes is sizeof(setup) + max payload, regardless of direction.

    // SET_IDLE(if=0)
    *setup = (usb_setup_packet_t){
        .bmRequestType = 0x21, .bRequest = 0x0A,
        .wValue = 0, .wIndex = 0, .wLength = 0,
    };
    xfer->bEndpointAddress = 0x00;
    xfer->num_bytes = 8;
    err = ctrl_submit_wait(xfer);
    ESP_LOGI(TAG, "SET_IDLE(if=0) -> %s", esp_err_to_name(err));

    // SET_IDLE(if=1)
    setup->wIndex = 1;
    err = ctrl_submit_wait(xfer);
    ESP_LOGI(TAG, "SET_IDLE(if=1) -> %s", esp_err_to_name(err));

    // SET_REPORT(if=1, output report 0, 1 byte payload of 0x00)
    *setup = (usb_setup_packet_t){
        .bmRequestType = 0x21, .bRequest = 0x09,
        .wValue = 0x0200, .wIndex = 1, .wLength = 1,
    };
    xfer->bEndpointAddress = 0x00;
    xfer->data_buffer[8] = 0x00;
    xfer->num_bytes = 8 + 1;
    err = ctrl_submit_wait(xfer);
    ESP_LOGI(TAG, "SET_REPORT(if=1) -> %s", esp_err_to_name(err));

    // GET_REPORT(if=0, input report 0, 512 bytes)
    *setup = (usb_setup_packet_t){
        .bmRequestType = 0xA1, .bRequest = 0x01,
        .wValue = 0x0100, .wIndex = 0, .wLength = 512,
    };
    xfer->bEndpointAddress = 0x80;        // IN
    xfer->num_bytes = 8 + 512;            // setup + room for response
    err = ctrl_submit_wait(xfer);
    ESP_LOGI(TAG, "GET_REPORT(if=0) -> %s (got %d bytes)",
             esp_err_to_name(err), xfer->actual_num_bytes - 8);

    usb_host_transfer_free(xfer);
    return ESP_OK;  // non-fatal: keep going even if any of these stall
}

static esp_err_t open_and_claim(uint8_t addr)
{
    esp_err_t err = usb_host_device_open(g_akp.client, addr, &g_akp.dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open dev: %s", esp_err_to_name(err));
        return err;
    }
    const usb_device_desc_t *dd;
    ESP_ERROR_CHECK(usb_host_get_device_descriptor(g_akp.dev, &dd));
    if (dd->idVendor != AKP03E_VID || dd->idProduct != AKP03E_REV2_PID) {
        ESP_LOGI(TAG, "ignoring device %04x:%04x", dd->idVendor, dd->idProduct);
        usb_host_device_close(g_akp.client, g_akp.dev);
        g_akp.dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "found AKP03E rev2 at addr %u", addr);

    err = usb_host_interface_claim(g_akp.client, g_akp.dev, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "claim iface: %s", esp_err_to_name(err));
        usb_host_device_close(g_akp.client, g_akp.dev);
        g_akp.dev = NULL;
        return err;
    }

    // Run the HID-class handshake the official software performs before any
    // interrupt-OUT writes.
    do_hid_init_handshake();

    ESP_ERROR_CHECK(usb_host_transfer_alloc(AKP03E_PACKET_SIZE,    0, &g_akp.out_xfer));
    ESP_ERROR_CHECK(usb_host_transfer_alloc(AKP03E_IN_PACKET_SIZE, 0, &g_akp.in_xfer));

    g_akp.in_xfer->device_handle = g_akp.dev;
    g_akp.in_xfer->bEndpointAddress = AKP03E_EP_IN;
    g_akp.in_xfer->num_bytes = AKP03E_IN_PACKET_SIZE;  // one IN report per URB (MPS=512)
    g_akp.in_xfer->callback = in_xfer_cb;
    g_akp.in_xfer->context = NULL;

    g_akp.attached = true;
    ESP_ERROR_CHECK(usb_host_transfer_submit(g_akp.in_xfer));

    if (g_akp.cb) {
        akp03e_event_t ev = { .type = AKP03E_EVT_CONNECTED };
        g_akp.cb(&ev, g_akp.cb_user);
    }
    return ESP_OK;
}

static void close_device(void)
{
    if (!g_akp.dev) return;
    g_akp.attached = false;

    if (g_akp.in_xfer)  { usb_host_transfer_free(g_akp.in_xfer);  g_akp.in_xfer  = NULL; }
    if (g_akp.out_xfer) { usb_host_transfer_free(g_akp.out_xfer); g_akp.out_xfer = NULL; }

    usb_host_interface_release(g_akp.client, g_akp.dev, 0);
    usb_host_device_close(g_akp.client, g_akp.dev);
    g_akp.dev = NULL;

    if (g_akp.cb) {
        akp03e_event_t ev = { .type = AKP03E_EVT_DISCONNECTED };
        g_akp.cb(&ev, g_akp.cb_user);
    }
}

// The client event callback runs inside usb_host_client_handle_events(),
// so it cannot block. It just enqueues the device address / handle for the
// dedicated device task to process. The client task only ever drives the
// event loop, so completion callbacks keep flowing while the device task
// blocks on transfer-completion semaphores.
typedef struct {
    enum { EVT_NEW_DEV, EVT_DEV_GONE } kind;
    uint8_t addr;
    usb_device_handle_t hdl;
} dev_event_t;
static QueueHandle_t s_dev_evt_q;

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    dev_event_t e = {0};
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        e.kind = EVT_NEW_DEV;
        e.addr = msg->new_dev.address;
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        e.kind = EVT_DEV_GONE;
        e.hdl = msg->dev_gone.dev_hdl;
    } else {
        return;
    }
    xQueueSendFromISR(s_dev_evt_q, &e, NULL);
}

static void client_task(void *arg)
{
    const usb_host_client_config_t cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 4,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&cfg, &g_akp.client));

    for (;;) {
        usb_host_client_handle_events(g_akp.client, portMAX_DELAY);
    }
}

static void device_task(void *arg)
{
    dev_event_t e;
    for (;;) {
        if (xQueueReceive(s_dev_evt_q, &e, portMAX_DELAY) != pdTRUE) continue;
        if (e.kind == EVT_NEW_DEV) {
            if (g_akp.dev == NULL) open_and_claim(e.addr);
        } else if (e.kind == EVT_DEV_GONE) {
            if (g_akp.dev == e.hdl) close_device();
        }
    }
}

esp_err_t akp03e_usb_start(void)
{
    g_akp.lock     = xSemaphoreCreateMutex();
    g_akp.out_done = xSemaphoreCreateBinary();
    s_ctrl_done    = xSemaphoreCreateBinary();
    s_dev_evt_q    = xQueueCreate(4, sizeof(dev_event_t));
    if (!g_akp.lock || !g_akp.out_done || !s_ctrl_done || !s_dev_evt_q) return ESP_ERR_NO_MEM;

    BaseType_t ok1 = xTaskCreatePinnedToCore(client_task, "akp_client", 6144, NULL, 6, NULL, 0);
    BaseType_t ok2 = xTaskCreatePinnedToCore(device_task, "akp_device", 6144, NULL, 5, NULL, 0);
    return (ok1 == pdPASS && ok2 == pdPASS) ? ESP_OK : ESP_FAIL;
}

// Forward declaration so akp03e.c implements the parsing in a single place.
extern void akp03e_handle_input(const uint8_t *data, size_t len);

static void parse_input_and_fire(const uint8_t *data, size_t len)
{
    akp03e_handle_input(data, len);
}
