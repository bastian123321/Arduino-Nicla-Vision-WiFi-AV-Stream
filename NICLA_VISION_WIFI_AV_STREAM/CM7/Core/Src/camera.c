/*
 * Camera: GC2145 sensor on DCMI, frames captured by DMA2 Stream 3
 *
 * The peripherals themselves (DCMI, DMA, I2C3, TIM3) are set up by CubeMX in
 * main.c; this file only starts them and runs the capture.
 *
 * Two capture modes:
 * - camera_capture(): one snapshot into one buffer (simple, but the sensor
 *   frame that starts while the caller processes the last one is missed).
 * - camera_stream_*(): continuous capture with the DMA in double-buffer mode.
 *   The hardware alternates between two frame buffers on its own, so every
 *   sensor frame is captured; the caller always gets the newest finished one.
 */
#include "camera.h"
#include "main.h"

extern DCMI_HandleTypeDef hdcmi;
extern DMA_HandleTypeDef hdma_dcmi;
extern TIM_HandleTypeDef htim3;

#define FRAME_WORDS     (CAMERA_FRAME_SIZE / 4U)

static volatile uint8_t frame_done;
static volatile uint8_t frame_error;

/* Continuous capture state (written from the DMA/DCMI interrupts) */
static uint8_t *stream_buf[2];
static volatile uint8_t streaming;
static volatile int8_t stream_ready = -1;   /* buffer holding the newest frame */
static volatile uint32_t stream_seq;        /* frames completed so far */
static volatile uint8_t stream_error;
static uint32_t taken_seq;

camera_status_t camera_init(gc2145_format_t fmt, uint16_t *chip_id)
{
  uint16_t id = 0;

  /* 12 MHz XCLK on PA7: the sensor needs it before it answers on I2C */
  if (HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_2) != HAL_OK)
  {
    return CAMERA_ERR_CONFIG;
  }
  HAL_Delay(10);

  if (gc2145_read_id(&id) != 0)
  {
    id = 0;
  }
  if (chip_id != NULL)
  {
    *chip_id = id;
  }
  if (id != GC2145_CHIP_ID)
  {
    return CAMERA_ERR_NO_SENSOR;
  }

  int ret = 0;
  ret |= gc2145_reset();
  ret |= gc2145_set_format(fmt);
  ret |= gc2145_set_framesize(CAMERA_WIDTH, CAMERA_HEIGHT);
  ret |= gc2145_set_orientation(0, 0);  // upright (sensor is mounted rotated 180°)
  if (ret != 0)
  {
    return CAMERA_ERR_CONFIG;
  }

  /* Let auto exposure / white balance settle a little */
  HAL_Delay(100);

  /* Line interrupts are not needed and would only cost CPU time */
  __HAL_DCMI_DISABLE_IT(&hdcmi, DCMI_IT_LINE);

  return CAMERA_OK;
}

camera_status_t camera_capture(uint8_t *buf, uint32_t timeout_ms)
{
  /*
   * The DMA writes RAM behind the D-cache. Drop any cached copy of the buffer
   * before the capture (so no dirty line gets written back over new pixels)
   * and again after it (so the CPU reads what the DMA wrote).
   */
  SCB_InvalidateDCache_by_Addr((uint32_t *)buf, CAMERA_FRAME_SIZE);

  frame_done = 0;
  frame_error = 0;

  if (HAL_DCMI_Start_DMA(&hdcmi, DCMI_MODE_SNAPSHOT, (uint32_t)buf,
                         CAMERA_FRAME_SIZE / 4U) != HAL_OK)
  {
    return CAMERA_ERR_BUSY;
  }

  uint32_t start = HAL_GetTick();
  while (!frame_done && !frame_error)
  {
    if ((HAL_GetTick() - start) > timeout_ms)
    {
      HAL_DCMI_Stop(&hdcmi);
      return CAMERA_ERR_TIMEOUT;
    }
  }

  if (frame_error)
  {
    HAL_DCMI_Stop(&hdcmi);
    return CAMERA_ERR_DCMI;
  }

  SCB_InvalidateDCache_by_Addr((uint32_t *)buf, CAMERA_FRAME_SIZE);
  return CAMERA_OK;
}

/* ---- Continuous capture ----------------------------------------------------- */

/* In double-buffer mode HAL reports which memory target just completed */
static void dma_m0_done(DMA_HandleTypeDef *h)
{
  (void)h;
  stream_ready = 0;
  stream_seq++;
}

static void dma_m1_done(DMA_HandleTypeDef *h)
{
  (void)h;
  stream_ready = 1;
  stream_seq++;
}

static void dma_error(DMA_HandleTypeDef *h)
{
  (void)h;
  stream_error = 1;
}

camera_status_t camera_stream_start(uint8_t *buf0, uint8_t *buf1)
{
  stream_buf[0] = buf0;
  stream_buf[1] = buf1;
  stream_ready = -1;
  stream_seq = 0;
  stream_error = 0;
  taken_seq = 0;

  /* The CPU never writes these buffers, so no dirty lines can overwrite pixels */
  SCB_InvalidateDCache_by_Addr((uint32_t *)buf0, CAMERA_FRAME_SIZE);
  SCB_InvalidateDCache_by_Addr((uint32_t *)buf1, CAMERA_FRAME_SIZE);

  hdma_dcmi.XferCpltCallback = dma_m0_done;
  hdma_dcmi.XferM1CpltCallback = dma_m1_done;
  hdma_dcmi.XferErrorCallback = dma_error;
  hdma_dcmi.XferHalfCpltCallback = NULL;
  hdma_dcmi.XferM1HalfCpltCallback = NULL;

  /* One frame per memory target; the DMA swaps targets at the end of each frame */
  if (HAL_DMAEx_MultiBufferStart_IT(&hdma_dcmi, (uint32_t)&DCMI->DR,
                                    (uint32_t)buf0, (uint32_t)buf1, FRAME_WORDS) != HAL_OK)
  {
    return CAMERA_ERR_BUSY;
  }

  hdcmi.State = HAL_DCMI_STATE_BUSY;
  __HAL_DCMI_CLEAR_FLAG(&hdcmi, DCMI_FLAG_FRAMERI | DCMI_FLAG_OVRRI | DCMI_FLAG_ERRRI |
                                DCMI_FLAG_VSYNCRI | DCMI_FLAG_LINERI);
  __HAL_DCMI_ENABLE_IT(&hdcmi, DCMI_IT_FRAME | DCMI_IT_OVR | DCMI_IT_ERR);
  MODIFY_REG(hdcmi.Instance->CR, DCMI_CR_CM, DCMI_MODE_CONTINUOUS);
  __HAL_DCMI_ENABLE(&hdcmi);
  hdcmi.Instance->CR |= DCMI_CR_CAPTURE;

  streaming = 1;
  return CAMERA_OK;
}

void camera_stream_stop(void)
{
  hdcmi.Instance->CR &= ~DCMI_CR_CAPTURE;
  __HAL_DCMI_DISABLE_IT(&hdcmi, DCMI_IT_FRAME | DCMI_IT_OVR | DCMI_IT_ERR |
                                DCMI_IT_VSYNC | DCMI_IT_LINE);
  __HAL_DCMI_DISABLE(&hdcmi);
  HAL_DMA_Abort(&hdma_dcmi);
  hdcmi.State = HAL_DCMI_STATE_READY;
  streaming = 0;
}

/*
 * Stop capturing and put the sensor in standby (XCLK off as well). The sensor
 * keeps its configuration, so camera_wake() only has to undo this.
 */
void camera_sleep(void)
{
  camera_stream_stop();
  gc2145_sleep(1);
  HAL_TIM_PWM_Stop(&htim3, TIM_CHANNEL_2);
}

camera_status_t camera_wake(uint8_t *buf0, uint8_t *buf1)
{
  if (HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_2) != HAL_OK)
  {
    return CAMERA_ERR_CONFIG;
  }
  HAL_Delay(2);  // let the sensor see a few XCLK cycles before talking to it
  if (gc2145_sleep(0) != 0)
  {
    return CAMERA_ERR_NO_SENSOR;
  }
  return camera_stream_start(buf0, buf1);
}

int camera_stream_failed(void)
{
  return stream_error != 0U;
}

uint32_t camera_stream_frames(void)
{
  return stream_seq;
}

const uint8_t *camera_stream_get(uint32_t *seq)
{
  uint32_t s = stream_seq;
  int8_t idx = stream_ready;

  if (!streaming || (idx < 0) || (s == taken_seq))
  {
    return NULL;  /* nothing new since the last call */
  }
  taken_seq = s;
  *seq = s;

  /* Drop stale cache lines so the CPU reads what the DMA wrote */
  SCB_InvalidateDCache_by_Addr((uint32_t *)stream_buf[idx], CAMERA_FRAME_SIZE);
  return stream_buf[idx];
}

int camera_stream_still_valid(uint32_t seq)
{
  /* As soon as the next frame completes, the DMA starts refilling this buffer */
  return stream_seq == seq;
}

/* ---- Interrupt callbacks ------------------------------------------------------- */

/* Called from HAL_DCMI_IRQHandler at the end of every frame */
void HAL_DCMI_FrameEventCallback(DCMI_HandleTypeDef *h)
{
  if (h->Instance != DCMI)
  {
    return;
  }
  if (!streaming)
  {
    frame_done = 1;
    return;
  }

  /*
   * At the end of a complete frame the DMA has just reloaded its counter. If
   * not, a frame was cut short (or ran long) and the buffers no longer line up
   * with sensor frames: have the main loop restart the stream.
   */
  if (((DMA_Stream_TypeDef *)hdma_dcmi.Instance)->NDTR != FRAME_WORDS)
  {
    stream_error = 1;
  }
}

void HAL_DCMI_ErrorCallback(DCMI_HandleTypeDef *h)
{
  if (h->Instance == DCMI)
  {
    frame_error = 1;
    stream_error = 1;
  }
}
