/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2024 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "usb_device.h"
#include "usbd_cdc_if.h"
#include "camera.h"
#include "jpeg_enc.h"
#include "usb_link.h"
#include "wifi.h"
#include "http_stream.h"
#include "cyw43_port.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* Start of the CM4 image in flash (must match CM4/STM32H747AIIX_FLASH.ld) */
#define CM4_IMAGE_ADDRESS    0x08100000U

#ifndef HSEM_ID_0
#define HSEM_ID_0 (0U) /* HW semaphore 0*/
#endif

/* JPEG quality 1..100: higher = sharper but bigger frames */
#define JPEG_QUALITY         60U

/* Camera goes to sleep this long after the last viewer left */
#define IDLE_AFTER_MS        3000U

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

DCMI_HandleTypeDef hdcmi;
DMA_HandleTypeDef hdma_dcmi;

I2C_HandleTypeDef hi2c2;
I2C_HandleTypeDef hi2c3;

JPEG_HandleTypeDef hjpeg;

SD_HandleTypeDef hsd2;

TIM_HandleTypeDef htim2;
TIM_HandleTypeDef htim3;

/* USER CODE BEGIN PV */
/*
 * Two QVGA frames for continuous capture. 32-byte aligned so the D-cache
 * maintenance covers exactly them.
 */
static uint8_t frame_buf[CAMERA_FRAME_SIZE] __attribute__((aligned(32)));
static uint8_t frame_buf2[CAMERA_FRAME_SIZE] __attribute__((aligned(32)));
/* Encoded frame; QVGA JPEGs at quality 60 are typically 3-10 KB */
static uint8_t jpeg_buf[32 * 1024] __attribute__((aligned(32)));
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_I2C2_Init(void);
static void MX_DCMI_Init(void);
static void MX_I2C3_Init(void);
static void MX_TIM3_Init(void);
static void MX_JPEG_Init(void);
static void MX_TIM2_Init(void);
/* USER CODE BEGIN PFP */
static void Bootloader_Handoff(void);
static void Disable_Caches(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  Bootloader_Handoff();

  // Release the M4 (it is held off by the option bytes), pointing it at its image
  __HAL_RCC_SYSCFG_CLK_ENABLE();
  HAL_SYSCFG_CM4BootAddConfig(SYSCFG_BOOT_ADDR0, CM4_IMAGE_ADDRESS);

  // Boot up the M4 core as is set to have it off by default using fuses
  HAL_RCCEx_EnableBootCore(RCC_BOOT_C2);

  /* USER CODE END 1 */
/* USER CODE BEGIN Boot_Mode_Sequence_0 */
  int32_t timeout;
/* USER CODE END Boot_Mode_Sequence_0 */

  /* Enable the CPU Cache */

  /* Enable I-Cache---------------------------------------------------------*/
  SCB_EnableICache();

  /* Enable D-Cache---------------------------------------------------------*/
  SCB_EnableDCache();

/* USER CODE BEGIN Boot_Mode_Sequence_1 */
  /* Wait until CPU2 boots and enters in stop mode or timeout*/
  timeout = 0xFFFF;
  while((__HAL_RCC_GET_FLAG(RCC_FLAG_D2CKRDY) != RESET) && (timeout-- > 0));
  if ( timeout < 0 )
  {
  Error_Handler();
  }
/* USER CODE END Boot_Mode_Sequence_1 */
  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* Enable the NICLA Vision oscillator pin */
  __HAL_RCC_GPIOH_CLK_ENABLE();
  GPIO_InitTypeDef  gpio_osc_init_structure;
  gpio_osc_init_structure.Pin = GPIO_PIN_1;
  gpio_osc_init_structure.Mode = GPIO_MODE_OUTPUT_PP;
  gpio_osc_init_structure.Pull = GPIO_PULLUP;
  gpio_osc_init_structure.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOH, &gpio_osc_init_structure);
  HAL_Delay(10);
  HAL_GPIO_WritePin(GPIOH, GPIO_PIN_1, 1);

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();
/* USER CODE BEGIN Boot_Mode_Sequence_2 */
/* When system initialization is finished, Cortex-M7 will release Cortex-M4 by means of
HSEM notification */
/*HW semaphore Clock enable*/
__HAL_RCC_HSEM_CLK_ENABLE();
/*Take HSEM */
HAL_HSEM_FastTake(HSEM_ID_0);
/*Release HSEM in order to notify the CPU2(CM4)*/
HAL_HSEM_Release(HSEM_ID_0,0);
/* wait until CPU2 wakes up from stop mode */
timeout = 0xFFFF;
while((__HAL_RCC_GET_FLAG(RCC_FLAG_D2CKRDY) == RESET) && (timeout-- > 0));
if ( timeout < 0 )
{
Error_Handler();
}
/* USER CODE END Boot_Mode_Sequence_2 */

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_I2C2_Init();
  MX_DCMI_Init();
  MX_I2C3_Init();
  MX_TIM3_Init();
  MX_JPEG_Init();
  MX_TIM2_Init();
  /* USER CODE BEGIN 2 */

  /* Setup power supply rails
   * This part enables all required rails on the power management module via I2C*/
  uint8_t data[2];

  // Charger LED Driver Enable controlled via software
  data[0]=0x9c;
  data[1]=0x80;
  HAL_I2C_Master_Transmit(&hi2c2, 0x08 << 1, data, sizeof(data), 100);

  // Charger LED Driver Enable controlled via software
  data[0]=0x9e;
  data[1]=0x20;
  HAL_I2C_Master_Transmit(&hi2c2, 0x08 << 1, data, sizeof(data), 100);

  // SW3 current limit at 1.5A
  data[0]=0x42;
  data[1]=0x02;
  HAL_I2C_Master_Transmit(&hi2c2, 0x08 << 1, data, sizeof(data), 100);

  // VBUS current limit at 1.5A
  data[0]=0x94;
  data[1]=0xa0;
  HAL_I2C_Master_Transmit(&hi2c2, 0x08 << 1, data, sizeof(data), 100);

  // LDO1
  data[0] = 0x4d;
  data[1] = 0x01;
  HAL_I2C_Master_Transmit(&hi2c2, 0x08 << 1, data, sizeof(data), 100);

  // LDO2
  data[0] = 0x50;
  data[1] = 0x01;
  HAL_I2C_Master_Transmit(&hi2c2, 0x08 << 1, data, sizeof(data), 100);

  // LDO3 to 1.2V
  data[0] = 0x53;
  data[1] = 0x01;
  HAL_I2C_Master_Transmit(&hi2c2, 0x08 << 1, data, sizeof(data), 100);

  // SW2
  data[0] = 0x3b;
  data[1] = 0x81;
  HAL_I2C_Master_Transmit(&hi2c2, 0x08 << 1, data, sizeof(data),100);

  // wait for the power lines to settle
  HAL_Delay(250);

  /* Enable USB PHY pin */
  HAL_GPIO_WritePin(USB_PHY_RST_GPIO_Port, USB_PHY_RST_Pin, SET);

  HAL_Delay(20);

  // Must be called only after 3.1V power supply has settled and the PA_2 is pulled high
  MX_USB_DEVICE_Init();

  /* Camera: needs the PMIC rails above, so it comes last */
  uint16_t cam_id = 0;
  camera_status_t cam_status = camera_init(GC2145_FMT_YUV422, &cam_id);
  int jpeg_status = jpeg_enc_init(CAMERA_WIDTH, CAMERA_HEIGHT, JPEG_QUALITY);
  if ((cam_status != CAMERA_OK) || (jpeg_status != 0))
  {
    HAL_GPIO_WritePin(LED_R_GPIO_Port, LED_R_Pin, GPIO_PIN_RESET);  // red on
  }

  /* WiFi: loads the chip firmware (~1 s), then joins in the background */
  int wifi_status = wifi_start();
  int http_status = (wifi_status == 0) ? http_stream_init() : -1;

  /*
   * Idle until someone watches: the camera sleeps and the CPU waits for
   * interrupts. A browser on /stream or /snapshot.jpg, or the USB viewer
   * (COM port open), wakes it; IDLE_AFTER_MS after the last one leaves it
   * goes back to sleep.
   */
  int cam_awake = 0;
  uint32_t last_watched = HAL_GetTick();
  if (cam_status == CAMERA_OK)
  {
    camera_sleep();
  }

  uint32_t stat_tick = HAL_GetTick();
  uint32_t cam_frames_last = 0;
  /* Per-second counters for the status line */
  uint32_t sent = 0, dropped = 0, jpeg_errors = 0, restarts = 0;
  uint32_t bytes_sum = 0, enc_ms_sum = 0;
  uint32_t sleep_us = 0;   /* time spent in __WFI, for the CPU load figure */

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    wifi_poll();

    /* Wake the camera for viewers, put it back to sleep when they're gone */
    int watched = (http_stream_clients() > 0) || CDC_Port_Is_Open();
    if (watched)
    {
      last_watched = HAL_GetTick();
    }
    if ((cam_status == CAMERA_OK) && watched && !cam_awake)
    {
      cam_status = camera_wake(frame_buf, frame_buf2);
      cam_awake = (cam_status == CAMERA_OK);
      cam_frames_last = 0;
    }
    else if (cam_awake && !watched && ((HAL_GetTick() - last_watched) > IDLE_AFTER_MS))
    {
      camera_sleep();
      cam_awake = 0;
      HAL_GPIO_WritePin(LED_G_GPIO_Port, LED_G_Pin, GPIO_PIN_SET);  // green off
    }

    if (cam_awake)
    {
      if (camera_stream_failed())
      {
        /* DCMI overrun or lost frame sync: start over */
        camera_stream_stop();
        camera_stream_start(frame_buf, frame_buf2);
        restarts++;
      }

      /* Encode the newest frame, unless the last one is still going out */
      uint32_t seq;
      const uint8_t *frame = http_stream_busy() ? NULL : camera_stream_get(&seq);
      if (frame != NULL)
      {
        uint32_t t0 = HAL_GetTick();
        uint32_t jpeg_len = 0;
        int ok = (jpeg_enc_encode(frame, jpeg_buf, sizeof(jpeg_buf), &jpeg_len) == 0);
        enc_ms_sum += HAL_GetTick() - t0;

        if (!ok)
        {
          jpeg_errors++;
        }
        else if (!camera_stream_still_valid(seq))
        {
          dropped++;   /* the DMA started refilling the buffer during encoding */
        }
        else
        {
          sent++;
          bytes_sum += jpeg_len;
          HAL_GPIO_TogglePin(LED_G_GPIO_Port, LED_G_Pin);
          http_stream_submit(jpeg_buf, jpeg_len);
          usb_link_send_frame(jpeg_buf, jpeg_len, CAMERA_WIDTH, CAMERA_HEIGHT,
                              USB_LINK_FMT_JPEG);
        }
      }
    }

    /* Status line once a second (also repeats the init results for late viewers) */
    if ((HAL_GetTick() - stat_tick) >= 1000U)
    {
      stat_tick = HAL_GetTick();
      uint32_t cam_frames = cam_awake ? camera_stream_frames() : 0U;
      uint32_t n = (sent > 0U) ? sent : 1U;
      if (cam_frames < cam_frames_last)
      {
        cam_frames_last = 0;   /* counter restarted with the stream */
      }
      uint32_t cpu_load = (sleep_us < 1000000U) ? (100U - (sleep_us / 10000U)) : 0U;
      usb_link_log("cam=%d id=0x%04X %s | camera %lu fps, sent %lu fps, %lu B/frame, encode %lu ms | "
                   "cpu %lu%% | dropped %lu, jpeg errors %lu, restarts %lu",
                   (int)cam_status, cam_id, cam_awake ? "streaming" : "idle",
                   cam_frames - cam_frames_last, sent, bytes_sum / n,
                   enc_ms_sum / n, cpu_load, dropped, jpeg_errors, restarts);
      if (wifi_is_up())
      {
        usb_link_log("wifi: %s | http=%d, %d viewer(s) | open http://%s.local/ or http://%s/",
                     wifi_status_text(), http_status, http_stream_clients(),
                     wifi_hostname(), wifi_ip_text());
      }
      else
      {
        usb_link_log("wifi start=%d: %s", wifi_status, wifi_status_text());
      }
      cam_frames_last = cam_frames;
      sent = dropped = jpeg_errors = 0;
      bytes_sum = enc_ms_sum = 0;
      restarts = 0;
      sleep_us = 0;
    }

    /*
     * Nothing left to do until the next interrupt: let the CPU sleep instead of
     * spinning. Every event that brings work wakes it: camera frame done (DMA),
     * WiFi data (WL_HOST_WAKE), USB, and the 1 ms SysTick for lwIP timers.
     * Interrupts are masked around __WFI so the handler runs after the time is
     * taken, which keeps the CPU load figure honest.
     */
    __disable_irq();
    uint32_t t_sleep = TIM2->CNT;
    __WFI();
    sleep_us += TIM2->CNT - t_sleep;
    __enable_irq();
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_BYPASS;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 5;
  RCC_OscInitStruct.PLL.PLLN = 192;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 20;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_2;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief DCMI Initialization Function
  * @param None
  * @retval None
  */
static void MX_DCMI_Init(void)
{

  /* USER CODE BEGIN DCMI_Init 0 */

  /* USER CODE END DCMI_Init 0 */

  /* USER CODE BEGIN DCMI_Init 1 */

  /* USER CODE END DCMI_Init 1 */
  hdcmi.Instance = DCMI;
  hdcmi.Init.SynchroMode = DCMI_SYNCHRO_HARDWARE;
  hdcmi.Init.PCKPolarity = DCMI_PCKPOLARITY_FALLING;
  hdcmi.Init.VSPolarity = DCMI_VSPOLARITY_LOW;
  hdcmi.Init.HSPolarity = DCMI_HSPOLARITY_LOW;
  hdcmi.Init.CaptureRate = DCMI_CR_ALL_FRAME;
  hdcmi.Init.ExtendedDataMode = DCMI_EXTEND_DATA_8B;
  hdcmi.Init.JPEGMode = DCMI_JPEG_DISABLE;
  hdcmi.Init.ByteSelectMode = DCMI_BSM_ALL;
  hdcmi.Init.ByteSelectStart = DCMI_OEBS_ODD;
  hdcmi.Init.LineSelectMode = DCMI_LSM_ALL;
  hdcmi.Init.LineSelectStart = DCMI_OELS_ODD;
  if (HAL_DCMI_Init(&hdcmi) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN DCMI_Init 2 */

  /* USER CODE END DCMI_Init 2 */

}

/**
  * @brief I2C2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C2_Init(void)
{

  /* USER CODE BEGIN I2C2_Init 0 */

  /* USER CODE END I2C2_Init 0 */

  /* USER CODE BEGIN I2C2_Init 1 */

  /* USER CODE END I2C2_Init 1 */
  hi2c2.Instance = I2C2;
  hi2c2.Init.Timing = 0x307075B1;
  hi2c2.Init.OwnAddress1 = 0;
  hi2c2.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c2.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c2.Init.OwnAddress2 = 0;
  hi2c2.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c2.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c2.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c2) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Analogue filter
  */
  if (HAL_I2CEx_ConfigAnalogFilter(&hi2c2, I2C_ANALOGFILTER_ENABLE) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Digital filter
  */
  if (HAL_I2CEx_ConfigDigitalFilter(&hi2c2, 0) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C2_Init 2 */

  /* USER CODE END I2C2_Init 2 */

}

/**
  * @brief I2C3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C3_Init(void)
{

  /* USER CODE BEGIN I2C3_Init 0 */

  /* USER CODE END I2C3_Init 0 */

  /* USER CODE BEGIN I2C3_Init 1 */

  /* USER CODE END I2C3_Init 1 */
  hi2c3.Instance = I2C3;
  hi2c3.Init.Timing = 0x307075B1;
  hi2c3.Init.OwnAddress1 = 0;
  hi2c3.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c3.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c3.Init.OwnAddress2 = 0;
  hi2c3.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c3.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c3.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c3) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Analogue filter
  */
  if (HAL_I2CEx_ConfigAnalogFilter(&hi2c3, I2C_ANALOGFILTER_ENABLE) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Digital filter
  */
  if (HAL_I2CEx_ConfigDigitalFilter(&hi2c3, 0) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C3_Init 2 */

  /* USER CODE END I2C3_Init 2 */

}

/**
  * @brief JPEG Initialization Function
  * @param None
  * @retval None
  */
static void MX_JPEG_Init(void)
{

  /* USER CODE BEGIN JPEG_Init 0 */

  /* USER CODE END JPEG_Init 0 */

  /* USER CODE BEGIN JPEG_Init 1 */

  /* USER CODE END JPEG_Init 1 */
  hjpeg.Instance = JPEG;
  if (HAL_JPEG_Init(&hjpeg) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN JPEG_Init 2 */

  /* USER CODE END JPEG_Init 2 */

}

/**
  * @brief SDMMC2 Initialization Function
  * @param None
  * @retval None
  */
void MX_SDMMC2_SD_Init(void)
{

  /* USER CODE BEGIN SDMMC2_Init 0 */

  /* USER CODE END SDMMC2_Init 0 */

  /* USER CODE BEGIN SDMMC2_Init 1 */

  /* USER CODE END SDMMC2_Init 1 */
  hsd2.Instance = SDMMC2;
  hsd2.Init.ClockEdge = SDMMC_CLOCK_EDGE_RISING;
  hsd2.Init.ClockPowerSave = SDMMC_CLOCK_POWER_SAVE_DISABLE;
  hsd2.Init.BusWide = SDMMC_BUS_WIDE_4B;
  hsd2.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_DISABLE;
  hsd2.Init.ClockDiv = 0;
  if (HAL_SD_Init(&hsd2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SDMMC2_Init 2 */

  /* USER CODE END SDMMC2_Init 2 */

}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 239;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 4294967295;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */

}

/**
  * @brief TIM3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM3_Init(void)
{

  /* USER CODE BEGIN TIM3_Init 0 */

  /* USER CODE END TIM3_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};

  /* USER CODE BEGIN TIM3_Init 1 */

  /* USER CODE END TIM3_Init 1 */
  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 0;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = 19;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
  if (HAL_TIM_Base_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim3, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 10;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&htim3, &sConfigOC, TIM_CHANNEL_2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM3_Init 2 */

  /* USER CODE END TIM3_Init 2 */
  HAL_TIM_MspPostInit(&htim3);

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA2_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA2_Stream3_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream3_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream3_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */
  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LED_R_GPIO_Port, LED_R_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LED_G_GPIO_Port, LED_G_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LED_B_GPIO_Port, LED_B_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(WL_REG_ON_GPIO_Port, WL_REG_ON_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(USB_PHY_RST_GPIO_Port, USB_PHY_RST_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : LED_R_Pin */
  GPIO_InitStruct.Pin = LED_R_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LED_R_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : LED_G_Pin */
  GPIO_InitStruct.Pin = LED_G_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LED_G_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : LED_B_Pin */
  GPIO_InitStruct.Pin = LED_B_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LED_B_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : WL_REG_ON_Pin */
  GPIO_InitStruct.Pin = WL_REG_ON_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(WL_REG_ON_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : WL_HOST_WAKE_Pin */
  GPIO_InitStruct.Pin = WL_HOST_WAKE_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(WL_HOST_WAKE_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : USB_PHY_RST_Pin */
  GPIO_InitStruct.Pin = USB_PHY_RST_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_PULLDOWN;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(USB_PHY_RST_GPIO_Port, &GPIO_InitStruct);

  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(WL_HOST_WAKE_EXTI_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(WL_HOST_WAKE_EXTI_IRQn);

  /* USER CODE BEGIN MX_GPIO_Init_2 */
  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/*
 * Built with -O2 on purpose: CMSIS 5.1.1 SCB_DisableDCache() disables the
 * cache before cleaning it, so at -O0 its loop counters live on the (cached)
 * stack, get read back stale from RAM and the loop never ends.
 */
__attribute__((optimize("O2"))) static void Disable_Caches(void)
{
  SCB_DisableICache();
  SCB_DisableDCache();
}

/*
 * The bootloader jumps here without a reset. Put VTOR, NVIC, caches, MPU and
 * the RCC peripheral resets / clock enables back to their power-on state.
 */
static void Bootloader_Handoff(void)
{
  extern uint32_t g_pfnVectors[];

  __disable_irq();

  /* Our own vector table, wherever we were linked */
  SCB->VTOR = (uint32_t)g_pfnVectors;
  __DSB();
  __ISB();

  /* No interrupts left over from the bootloader */
  SysTick->CTRL = 0;
  for (uint32_t i = 0; i < 8; i++)
  {
    NVIC->ICER[i] = 0xFFFFFFFFU;
    NVIC->ICPR[i] = 0xFFFFFFFFU;
  }

  /* Caches off and MPU off, as after reset */
  Disable_Caches();
  HAL_MPU_Disable();

  /* Reset every peripheral the bootloader may have used */
  __HAL_RCC_AHB1_FORCE_RESET();
  __HAL_RCC_AHB2_FORCE_RESET();
  __HAL_RCC_AHB3_FORCE_RESET();
  __HAL_RCC_AHB4_FORCE_RESET();
  __HAL_RCC_APB1L_FORCE_RESET();
  __HAL_RCC_APB1H_FORCE_RESET();
  __HAL_RCC_APB2_FORCE_RESET();
  __HAL_RCC_APB3_FORCE_RESET();
  __HAL_RCC_APB4_FORCE_RESET();
  __HAL_RCC_AHB1_RELEASE_RESET();
  __HAL_RCC_AHB2_RELEASE_RESET();
  __HAL_RCC_AHB3_RELEASE_RESET();
  __HAL_RCC_AHB4_RELEASE_RESET();
  __HAL_RCC_APB1L_RELEASE_RESET();
  __HAL_RCC_APB1H_RELEASE_RESET();
  __HAL_RCC_APB2_RELEASE_RESET();
  __HAL_RCC_APB3_RELEASE_RESET();
  __HAL_RCC_APB4_RELEASE_RESET();

  /* Give up CM7's clock enables so D2 can go to STOP.
   * AHB3ENR also holds the FLASH/TCM/AXI SRAM bits: only touch peripherals. */
  RCC->AHB1ENR  = 0;
  RCC->AHB2ENR  = 0;
  RCC->AHB3ENR &= ~(RCC_AHB3ENR_MDMAEN | RCC_AHB3ENR_DMA2DEN | RCC_AHB3ENR_JPGDECEN |
                    RCC_AHB3ENR_FMCEN | RCC_AHB3ENR_QSPIEN | RCC_AHB3ENR_SDMMC1EN);
  RCC->AHB4ENR  = 0;
  RCC->APB1LENR = 0;
  RCC->APB1HENR = 0;
  RCC->APB2ENR  = 0;
  RCC->APB3ENR  = 0;
  RCC->APB4ENR  = RCC_APB4ENR_RTCAPBEN;
  __DSB();

  __enable_irq();
}

/* GPIO EXTI interrupts (EXTI15_10_IRQHandler in stm32h7xx_it.c lands here) */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == WL_HOST_WAKE_Pin)
  {
    cyw43_port_host_wake_irq();   /* WiFi chip has data for us */
  }
}

/*
 * @brief  Reboot into the Arduino bootloader (DFU mode)
 *
 * Called on the "1200 baud touch" (see usbd_cdc_if.c), so tools/upload.ps1 can
 * flash the board without a double-tap on reset. The bootloader stays in DFU
 * mode instead of starting the application when it finds 0xDF59 in RTC->BKP0R.
 */
void Enter_Arduino_Bootloader(void)
{
  __disable_irq();

  HAL_PWR_EnableBkUpAccess();
  __HAL_RCC_RTC_CLK_ENABLE();
  RTC->BKP0R = ARDUINO_BOOTLOADER_MAGIC;
  __DSB();

  NVIC_SystemReset();
}
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
