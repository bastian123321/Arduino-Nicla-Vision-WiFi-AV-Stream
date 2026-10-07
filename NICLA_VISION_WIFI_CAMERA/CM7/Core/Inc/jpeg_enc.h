/*
 * JPEG encoding of YUV422 camera frames with the STM32H7 hardware codec
 */
#ifndef JPEG_ENC_H
#define JPEG_ENC_H

#include <stdint.h>

/* Set image size and quality (1..100). Call once after MX_JPEG_Init(). */
int jpeg_enc_init(uint16_t width, uint16_t height, uint8_t quality);

/*
 * Encode one YUYV (Y0 Cb Y1 Cr) frame into out. The frame is fed to the codec
 * one 8-line MCU row at a time, so no second full-size buffer is needed.
 * Returns 0 and the JPEG size in *out_len, or -1 (error / out too small).
 */
int jpeg_enc_encode(const uint8_t *yuyv, uint8_t *out, uint32_t out_max, uint32_t *out_len);

#endif /* JPEG_ENC_H */
