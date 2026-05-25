#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// Hardware JPEG encoder helper for fixed 64x64 RGB888 inputs (size of the
// AKP03E LCD keys). Owns a single DMA-capable input buffer and output buffer
// internally — only one encode at a time, single-task callers only.

#define JPEG_ENC_W 64
#define JPEG_ENC_H 64
#define JPEG_ENC_IN_SIZE (JPEG_ENC_W * JPEG_ENC_H * 3)

esp_err_t jpeg_enc_init(void);

// Returns a writable pointer to the input buffer (JPEG_ENC_IN_SIZE bytes,
// RGB888 row-major). Caller fills, then calls jpeg_enc_encode().
uint8_t *jpeg_enc_input_buf(void);

// Encode the current contents of the input buffer. On success returns a
// pointer to the JPEG bytes in the internal output buffer and writes the
// byte length to *out_size. Returned pointer is valid until the next encode
// call. Returns NULL on failure.
const uint8_t *jpeg_enc_encode(size_t *out_size);
