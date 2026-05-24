#include "jpeg_enc.h"

#include "driver/jpeg_encode.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "jpeg_enc";

// 60x60 q85 RGB888 -> JPEG ~2-4 KB in practice; 8 KB is comfortable headroom.
#define JPEG_OUT_BUF_SIZE 8192

static jpeg_encoder_handle_t s_enc;
static uint8_t *s_in_buf;       // DMA-capable, JPEG_ENC_IN_SIZE bytes
static uint8_t *s_out_buf;      // DMA-capable, JPEG_OUT_BUF_SIZE bytes
static size_t s_in_alloc;
static size_t s_out_alloc;

esp_err_t jpeg_enc_init(void)
{
    if (s_enc) return ESP_OK;

    const jpeg_encode_engine_cfg_t cfg = {
        .intr_priority = 0,
        .timeout_ms = 1000,
    };
    esp_err_t err = jpeg_new_encoder_engine(&cfg, &s_enc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new encoder: %s", esp_err_to_name(err));
        return err;
    }

    const jpeg_encode_memory_alloc_cfg_t in_cfg  = { .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER };
    const jpeg_encode_memory_alloc_cfg_t out_cfg = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    s_in_buf  = jpeg_alloc_encoder_mem(JPEG_ENC_IN_SIZE,    &in_cfg,  &s_in_alloc);
    s_out_buf = jpeg_alloc_encoder_mem(JPEG_OUT_BUF_SIZE,   &out_cfg, &s_out_alloc);
    if (!s_in_buf || !s_out_buf) {
        ESP_LOGE(TAG, "alloc encoder mem failed (in=%p out=%p)", s_in_buf, s_out_buf);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ready (in=%zu B, out=%zu B)", s_in_alloc, s_out_alloc);
    return ESP_OK;
}

uint8_t *jpeg_enc_input_buf(void)
{
    return s_in_buf;
}

const uint8_t *jpeg_enc_encode(size_t *out_size)
{
    if (!s_enc || !s_in_buf || !s_out_buf || !out_size) return NULL;

    const jpeg_encode_cfg_t enc_cfg = {
        .width  = JPEG_ENC_W,
        .height = JPEG_ENC_H,
        .src_type    = JPEG_ENC_SRC_RGB888,
        .sub_sample  = JPEG_DOWN_SAMPLING_YUV444,
        .image_quality = 85,
    };
    uint32_t produced = 0;
    esp_err_t err = jpeg_encoder_process(s_enc, &enc_cfg,
                                          s_in_buf, JPEG_ENC_IN_SIZE,
                                          s_out_buf, s_out_alloc,
                                          &produced);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "encode: %s", esp_err_to_name(err));
        return NULL;
    }
    *out_size = produced;
    return s_out_buf;
}
