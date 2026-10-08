/*
 * Microphone: MP34DT06J PDM mic on DFSDM1, 16 kHz mono (see audio.h)
 *
 * DFSDM1, its pins and DMA1 Stream 0 are configured in the .ioc
 * (MX_DFSDM1_Init); this file only starts the conversion and handles the data.
 * Filter settings follow OpenMV's DFSDM audio driver (MIT), which Arduino
 * also uses on this board: 2 MHz mic clock, FastSinc, oversampling 125.
 */
#include "audio.h"
#include <math.h>
#include <string.h>
#include "main.h"

extern DFSDM_Filter_HandleTypeDef hdfsdm1_filter0;

/*
 * Digital gain. FastSinc with oversampling 125 gives at most +-31250, so the
 * raw samples already span the 16-bit range; the mic's -26 dBFS sensitivity
 * makes normal speech quiet though, hence some amplification.
 */
#define AUDIO_GAIN            8

/* Skip the first blocks after start: the mic needs ~10 ms to wake up and the
 * high-pass filter needs a moment to settle */
#define STARTUP_BLOCKS        10U

/* FIFO between the DMA interrupt and the main loop: 8192 samples = 512 ms.
 * A power of two, so the free-running indices stay correct when they wrap. */
#define FIFO_SAMPLES          8192U

/* DMA ring: two halves of one block each. 32-byte aligned and a multiple of
 * 32 bytes, so the D-cache maintenance covers exactly this buffer. */
static int32_t dma_buf[2U * AUDIO_BLOCK_SAMPLES] __attribute__((aligned(32)));

static int16_t fifo[FIFO_SAMPLES];
static volatile uint32_t fifo_head;   /* written by the interrupt */
static volatile uint32_t fifo_tail;   /* written by the main loop */

static volatile uint8_t running;
static volatile uint32_t blocks_seen;
static volatile uint32_t dropped;
static volatile int32_t peak;

/* High-pass filter state (removes the mic's DC offset) */
static int32_t hp_x1;
static int32_t hp_y1;

static void process_block(const int32_t *raw)
{
  int16_t pcm[AUDIO_BLOCK_SAMPLES];
  int32_t blk_peak = 0;

  /* The data register holds a 24-bit signed sample in bits 31:8 */
  SCB_InvalidateDCache_by_Addr((uint32_t *)raw, AUDIO_BLOCK_SAMPLES * sizeof(int32_t));

  for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
  {
    int32_t x = raw[i] >> 8;

    /* y[n] = x[n] - x[n-1] + 0.995 * y[n-1]  (corner ~13 Hz at 16 kHz) */
    int32_t y = x - hp_x1 + ((hp_y1 * 32604) >> 15);
    hp_x1 = x;
    hp_y1 = y;

    int32_t s = y * AUDIO_GAIN;
    if (s > 32767)
    {
      s = 32767;
    }
    else if (s < -32768)
    {
      s = -32768;
    }
    pcm[i] = (int16_t)s;

    int32_t a = (s < 0) ? -s : s;
    if (a > blk_peak)
    {
      blk_peak = a;
    }
  }

  if (++blocks_seen <= STARTUP_BLOCKS)
  {
    return;
  }
  if (blk_peak > peak)
  {
    peak = blk_peak;
  }

  uint32_t head = fifo_head;
  if ((head - fifo_tail) > (FIFO_SAMPLES - AUDIO_BLOCK_SAMPLES))
  {
    dropped++;   /* nobody is reading fast enough: drop this block */
    return;
  }
  for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
  {
    fifo[(head + i) % FIFO_SAMPLES] = pcm[i];
  }
  fifo_head = head + AUDIO_BLOCK_SAMPLES;
}

/* DMA half / full transfer: the other half is being written meanwhile */
void HAL_DFSDM_FilterRegConvHalfCpltCallback(DFSDM_Filter_HandleTypeDef *h)
{
  if (h == &hdfsdm1_filter0)
  {
    process_block(&dma_buf[0]);
  }
}

void HAL_DFSDM_FilterRegConvCpltCallback(DFSDM_Filter_HandleTypeDef *h)
{
  if (h == &hdfsdm1_filter0)
  {
    process_block(&dma_buf[AUDIO_BLOCK_SAMPLES]);
  }
}

int audio_start(void)
{
  if (running)
  {
    return 0;
  }
  hp_x1 = 0;
  hp_y1 = 0;
  blocks_seen = 0;
  fifo_tail = fifo_head;

  SCB_InvalidateDCache_by_Addr((uint32_t *)dma_buf, sizeof(dma_buf));
  if (HAL_DFSDM_FilterRegularStart_DMA(&hdfsdm1_filter0, dma_buf,
                                       2U * AUDIO_BLOCK_SAMPLES) != HAL_OK)
  {
    return -1;
  }
  running = 1;
  return 0;
}

void audio_stop(void)
{
  if (!running)
  {
    return;
  }
  HAL_DFSDM_FilterRegularStop_DMA(&hdfsdm1_filter0);
  running = 0;
}

int audio_running(void)
{
  return running;
}

uint32_t audio_available(void)
{
  return fifo_head - fifo_tail;
}

uint32_t audio_read(int16_t *out, uint32_t max)
{
  uint32_t tail = fifo_tail;
  uint32_t n = fifo_head - tail;

  if (n > max)
  {
    n = max;
  }
  for (uint32_t i = 0; i < n; i++)
  {
    out[i] = fifo[(tail + i) % FIFO_SAMPLES];
  }
  fifo_tail = tail + n;
  return n;
}

void audio_flush(void)
{
  fifo_tail = fifo_head;
}

void audio_take_stats(int *peak_dbfs, uint32_t *dropped_blocks)
{
  int32_t p = peak;
  peak = 0;
  *dropped_blocks = dropped;
  dropped = 0;

  *peak_dbfs = (p > 0) ? (int)lroundf(20.0f * log10f((float)p / 32768.0f)) : -96;
}
