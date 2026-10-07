/*
 * Camera: GC2145 sensor on DCMI, frames captured by DMA2 Stream 3
 */
#ifndef CAMERA_H
#define CAMERA_H

#include <stdint.h>
#include "gc2145.h"

#define CAMERA_WIDTH        320U
#define CAMERA_HEIGHT       240U
#define CAMERA_BPP          2U      /* RGB565 and YUV422 are both 2 bytes/pixel */
#define CAMERA_FRAME_SIZE   (CAMERA_WIDTH * CAMERA_HEIGHT * CAMERA_BPP)

typedef enum
{
  CAMERA_OK = 0,
  CAMERA_ERR_NO_SENSOR = -1,   /* no answer on I2C3 or wrong chip ID */
  CAMERA_ERR_CONFIG    = -2,   /* register writes failed */
  CAMERA_ERR_BUSY      = -3,   /* DCMI/DMA could not be started */
  CAMERA_ERR_TIMEOUT   = -4,   /* no complete frame in time */
  CAMERA_ERR_DCMI      = -5,   /* DCMI overrun / sync error */
} camera_status_t;

/* Start XCLK, probe and configure the sensor. Call after the PMIC rails are on. */
camera_status_t camera_init(gc2145_format_t fmt, uint16_t *chip_id);

/*
 * Capture one frame into buf (CAMERA_FRAME_SIZE bytes, 32-byte aligned,
 * in AXI SRAM). Blocks until the frame is complete or timeout_ms passes.
 */
camera_status_t camera_capture(uint8_t *buf, uint32_t timeout_ms);

/*
 * Continuous capture into two frame buffers (same requirements as above),
 * alternated by the DMA in hardware. Usage:
 *
 *   const uint8_t *f = camera_stream_get(&seq);   // newest frame, or NULL
 *   ...process f...
 *   if (!camera_stream_still_valid(seq)) -> f was being overwritten: drop result
 *
 * Processing must finish within one frame period (~35 ms) after
 * camera_stream_get(), because the DMA then starts refilling that buffer.
 */
camera_status_t camera_stream_start(uint8_t *buf0, uint8_t *buf1);
void camera_stream_stop(void);
const uint8_t *camera_stream_get(uint32_t *seq);
int camera_stream_still_valid(uint32_t seq);
uint32_t camera_stream_frames(void);   /* total frames captured */
int camera_stream_failed(void);        /* DCMI/DMA error or lost sync: restart */

/* Idle: stop capture, sensor standby, XCLK off. Wake restarts the stream. */
void camera_sleep(void);
camera_status_t camera_wake(uint8_t *buf0, uint8_t *buf1);

#endif /* CAMERA_H */
