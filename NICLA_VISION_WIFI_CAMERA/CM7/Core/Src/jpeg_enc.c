/*
 * JPEG encoding of YUV422 camera frames with the STM32H7 hardware codec
 *
 * The codec takes its input as MCUs. For YCbCr 4:2:2 an MCU covers 16x8
 * pixels and is 256 bytes: two 8x8 Y blocks (left, right), then one 8x8 Cb
 * and one 8x8 Cr block. HAL_JPEG_Encode() runs in polling mode and asks for
 * more input through HAL_JPEG_GetDataCallback(); each call converts the next
 * row of MCUs (8 image lines) from the camera's YUYV layout.
 */
#include "jpeg_enc.h"
#include "gc2145.h"
#include "main.h"

extern JPEG_HandleTypeDef hjpeg;

#define MCU_W            16U
#define MCU_H            8U
#define MCU_BYTES        256U
#define MAX_WIDTH        640U
#define ENCODE_TIMEOUT   200U   /* ms */

/*
 * Byte offsets of Cb and Cr inside each 4-byte pixel pair. With the 180°
 * rotation (GC2145_ROTATE) the sensor's horizontal mirror reverses the
 * pixel order, so it sends Y Cr Y Cb instead of Y Cb Y Cr.
 */
#if (GC2145_ROTATE == 1)
#define CB_OFFSET        3U
#define CR_OFFSET        1U
#else
#define CB_OFFSET        1U
#define CR_OFFSET        3U
#endif

/* One row of MCUs for the widest supported image */
static uint8_t mcu_row[(MAX_WIDTH / MCU_W) * MCU_BYTES] __attribute__((aligned(4)));

static struct
{
  const uint8_t *src;
  uint16_t width;
  uint16_t height;
  uint32_t row;          /* MCU row currently handed to the codec */
  uint32_t rows;
  uint32_t out_len;
  uint32_t out_chunks;   /* > 1 means the output buffer overflowed */
} enc;

/* Convert MCU row `row` of the YUYV frame into mcu_row[] */
static void convert_mcu_row(uint32_t row)
{
  const uint32_t w = enc.width;
  uint8_t *dst = mcu_row;

  for (uint32_t mx = 0; mx < w; mx += MCU_W)
  {
    uint8_t *y0 = dst;
    uint8_t *y1 = dst + 64;
    uint8_t *cb = dst + 128;
    uint8_t *cr = dst + 192;

    for (uint32_t r = 0; r < MCU_H; r++)
    {
      /* 16 pixels = 8 YUYV pairs = 32 bytes */
      const uint8_t *p = enc.src + (((row * MCU_H) + r) * w + mx) * 2U;
      uint8_t *yl = y0 + (r * 8U);
      uint8_t *yr = y1 + (r * 8U);

      for (uint32_t c = 0; c < 4; c++)
      {
        yl[2 * c]     = p[4 * c];
        yl[2 * c + 1] = p[4 * c + 2];
        yr[2 * c]     = p[16 + 4 * c];
        yr[2 * c + 1] = p[16 + 4 * c + 2];
      }
      for (uint32_t c = 0; c < 8; c++)
      {
        cb[r * 8U + c] = p[4 * c + CB_OFFSET];
        cr[r * 8U + c] = p[4 * c + CR_OFFSET];
      }
    }
    dst += MCU_BYTES;
  }
}

int jpeg_enc_init(uint16_t width, uint16_t height, uint8_t quality)
{
  JPEG_ConfTypeDef conf = {0};

  if ((width % MCU_W) || (height % MCU_H) || (width > MAX_WIDTH))
  {
    return -1;
  }

  conf.ColorSpace = JPEG_YCBCR_COLORSPACE;
  conf.ChromaSubsampling = JPEG_422_SUBSAMPLING;
  conf.ImageWidth = width;
  conf.ImageHeight = height;
  conf.ImageQuality = quality;

  enc.width = width;
  enc.height = height;
  enc.rows = height / MCU_H;

  return (HAL_JPEG_ConfigEncoding(&hjpeg, &conf) == HAL_OK) ? 0 : -1;
}

int jpeg_enc_encode(const uint8_t *yuyv, uint8_t *out, uint32_t out_max, uint32_t *out_len)
{
  const uint32_t row_bytes = (enc.width / MCU_W) * MCU_BYTES;

  enc.src = yuyv;
  enc.row = 0;
  enc.out_len = 0;
  enc.out_chunks = 0;
  convert_mcu_row(0);

  if (HAL_JPEG_Encode(&hjpeg, mcu_row, row_bytes, out, out_max, ENCODE_TIMEOUT) != HAL_OK)
  {
    return -1;
  }
  if (enc.out_chunks != 1U)
  {
    return -1;  /* output didn't fit: HAL wrapped around inside out[] */
  }

  *out_len = enc.out_len;
  return 0;
}

/* The codec has consumed the current input buffer: hand it the next MCU row */
void HAL_JPEG_GetDataCallback(JPEG_HandleTypeDef *h, uint32_t NbDecodedData)
{
  (void)NbDecodedData;

  if (++enc.row < enc.rows)
  {
    convert_mcu_row(enc.row);
    HAL_JPEG_ConfigInputBuffer(h, mcu_row, (enc.width / MCU_W) * MCU_BYTES);
  }
  else
  {
    HAL_JPEG_ConfigInputBuffer(h, mcu_row, 0);  /* no more input */
  }
}

/* Called once at the end of the image (or each time `out` is full) */
void HAL_JPEG_DataReadyCallback(JPEG_HandleTypeDef *h, uint8_t *pDataOut, uint32_t OutDataLength)
{
  (void)h;
  (void)pDataOut;

  enc.out_len += OutDataLength;
  enc.out_chunks++;
}
