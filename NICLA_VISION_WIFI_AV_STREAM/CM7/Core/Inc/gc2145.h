/*
 * GC2145 image sensor driver (Nicla Vision, I2C3 + DCMI)
 */
#ifndef GC2145_H
#define GC2145_H

#include <stdint.h>

/* 8-bit (HAL style) I2C address; 7-bit address is 0x3C */
#define GC2145_I2C_ADDR       0x78U

/* Chip ID read back from registers 0xF0/0xF1 */
#define GC2145_CHIP_ID        0x2145U

/* The Nicla Vision has the sensor mounted upside down: rotate 180° in the sensor */
#ifndef GC2145_ROTATE
#define GC2145_ROTATE         1
#endif

typedef enum
{
  GC2145_FMT_RGB565 = 0,
  GC2145_FMT_YUV422,
} gc2145_format_t;

int gc2145_read_id(uint16_t *id);
int gc2145_reset(void);
int gc2145_set_format(gc2145_format_t fmt);
int gc2145_set_framesize(uint16_t w, uint16_t h);
int gc2145_set_orientation(int hmirror, int vflip);  /* 0, 0 = upright */
int gc2145_sleep(int enable);                        /* 1 = standby, 0 = run */

#endif /* GC2145_H */
