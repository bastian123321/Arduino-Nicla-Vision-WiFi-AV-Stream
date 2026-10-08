/*
 * cyw43-driver port for the Nicla Vision: pins, time base, MAC address,
 * host-wake interrupt and log capture. See cyw43_configport.h.
 *
 * Hardware, all configured in the .ioc:
 *   TIM2           free-running 1 MHz counter (Prescaler 239) for cyw43_hal_ticks_us()
 *   WL_HOST_WAKE   PD15, EXTI falling edge -> HAL_GPIO_EXTI_Callback() in main.c
 *                  calls cyw43_port_host_wake_irq() to schedule a driver poll
 *   WL_REG_ON      PG4, output, starts low (chip off)
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "main.h"
#include "net/cyw43/cyw43.h"
#include "cyw43_port.h"

extern TIM_HandleTypeDef htim2;

const char cyw43_port_hostname[] = "nicla-vision";

/* Set from interrupts / the driver, consumed by cyw43_port_poll() */
static volatile uint8_t poll_pending;
static void (*volatile poll_func)(void);

/* ---- Pins ---------------------------------------------------------------- */

static GPIO_TypeDef *pin_port(cyw43_hal_pin_obj_t pin)
{
  return (GPIO_TypeDef *)(GPIOA_BASE + ((pin >> 4) * (GPIOB_BASE - GPIOA_BASE)));
}

static uint32_t pin_mask(cyw43_hal_pin_obj_t pin)
{
  return 1UL << (pin & 0x0FU);
}

/*
 * The driver asks for WL_REG_ON as output and WL_HOST_WAKE as input. Both are
 * already set up that way by MX_GPIO_Init() (WL_HOST_WAKE as falling-edge EXTI,
 * which reads like an input), so there is nothing to do: reconfiguring here
 * would only override the .ioc settings.
 */
void cyw43_hal_pin_config(cyw43_hal_pin_obj_t pin, uint32_t mode, uint32_t pull, uint32_t alt)
{
  (void)pin;
  (void)mode;
  (void)pull;
  (void)alt;
}

int cyw43_hal_pin_read(cyw43_hal_pin_obj_t pin)
{
  return (pin_port(pin)->IDR & pin_mask(pin)) ? 1 : 0;
}

void cyw43_hal_pin_low(cyw43_hal_pin_obj_t pin)
{
  pin_port(pin)->BSRR = pin_mask(pin) << 16;
}

void cyw43_hal_pin_high(cyw43_hal_pin_obj_t pin)
{
  pin_port(pin)->BSRR = pin_mask(pin);
}

/* WL_HOST_WAKE falling edge = chip has work for us. EXTI itself is set up by CubeMX. */
void cyw43_hal_pin_config_irq_falling(cyw43_hal_pin_obj_t pin, int enable)
{
  if (pin != CYW43_PIN_WL_HOST_WAKE)
  {
    return;
  }
  if (enable)
  {
    HAL_NVIC_EnableIRQ(WL_HOST_WAKE_EXTI_IRQn);
  }
  else
  {
    HAL_NVIC_DisableIRQ(WL_HOST_WAKE_EXTI_IRQn);
  }
}

void cyw43_port_host_wake_irq(void)
{
  poll_pending = 1;
}

/* ---- Time ---------------------------------------------------------------- */

static void ticks_us_init(void)
{
  /* TIM2 is configured by MX_TIM2_Init() for 1 MHz; it only needs starting */
  if (!(htim2.Instance->CR1 & TIM_CR1_CEN))
  {
    HAL_TIM_Base_Start(&htim2);
  }
}

uint32_t cyw43_hal_ticks_us(void)
{
  return TIM2->CNT;
}

uint32_t cyw43_hal_ticks_ms(void)
{
  return HAL_GetTick();
}

void cyw43_delay_us(uint32_t us)
{
  uint32_t start = TIM2->CNT;
  while ((TIM2->CNT - start) < us)
  {
  }
}

void cyw43_delay_ms(uint32_t ms)
{
  cyw43_delay_us(ms * 1000U);
}

/* ---- MAC address ---------------------------------------------------------- */

/* Locally administered, unicast, stable per board (from the 96-bit unique ID) */
void cyw43_hal_generate_laa_mac(int idx, uint8_t buf[6])
{
  uint32_t uid0 = HAL_GetUIDw0();
  uint32_t uid1 = HAL_GetUIDw1();
  uint32_t uid2 = HAL_GetUIDw2();
  uint32_t h = uid0 ^ (uid1 * 31U) ^ (uid2 * 131U);

  buf[0] = 0x02;                      // locally administered, unicast
  buf[1] = (uint8_t)(uid2 >> 8);
  buf[2] = (uint8_t)(h >> 24);
  buf[3] = (uint8_t)(h >> 16);
  buf[4] = (uint8_t)(h >> 8);
  buf[5] = (uint8_t)((h & 0xFCU) | (idx & 0x03));
}

void cyw43_hal_get_mac(int idx, uint8_t buf[6])
{
  cyw43_hal_generate_laa_mac(idx, buf);
}

/* ---- Poll scheduling ------------------------------------------------------ */

void cyw43_schedule_internal_poll_dispatch(void (*func)(void))
{
  poll_func = func;
  poll_pending = 1;
}

void cyw43_port_init(void)
{
  ticks_us_init();
}

void cyw43_port_poll(void)
{
  static uint32_t last_ms;
  uint32_t now = HAL_GetTick();

  if (cyw43_poll == NULL)
  {
    last_ms = now;
    return;
  }

  /* The chip pulls WL_HOST_WAKE low while it has data: don't rely on the edge alone */
  if (poll_pending || (cyw43_hal_pin_read(CYW43_PIN_WL_HOST_WAKE) == 0))
  {
    poll_pending = 0;
    if (poll_func != NULL)
    {
      void (*f)(void) = poll_func;
      poll_func = NULL;
      f();
    }
    else
    {
      cyw43_poll();
    }
  }

  /*
   * Bus sleep countdown, as in MicroPython: once the driver has been idle for
   * CYW43_SLEEP_MAX ms, one more poll puts the WLAN bus to sleep.
   */
  for (; last_ms != now; last_ms++)
  {
    if ((cyw43_sleep != 0U) && (--cyw43_sleep == 0U))
    {
      cyw43_poll();
    }
  }
}

/* ---- Log capture ---------------------------------------------------------- */

static char log_buf[1024];
static size_t log_len;

void cyw43_port_printf(const char *fmt, ...)
{
  va_list args;
  size_t room = sizeof(log_buf) - log_len;

  if (room <= 1U)
  {
    return;  // full: drop until the main loop drains it
  }
  va_start(args, fmt);
  int n = vsnprintf(log_buf + log_len, room, fmt, args);
  va_end(args);
  if (n > 0)
  {
    log_len += ((size_t)n < room) ? (size_t)n : (room - 1U);
  }
}

size_t cyw43_port_take_log_line(char *line, size_t max)
{
  char *nl = memchr(log_buf, '\n', log_len);
  size_t n;

  if (nl != NULL)
  {
    n = (size_t)(nl - log_buf);
  }
  else if (log_len >= (sizeof(log_buf) - 1U))
  {
    n = log_len;   // full without a newline: flush it anyway
  }
  else
  {
    return 0;
  }

  size_t copy = (n < (max - 1U)) ? n : (max - 1U);
  memcpy(line, log_buf, copy);
  line[copy] = '\0';

  size_t consumed = (nl != NULL) ? (n + 1U) : n;
  memmove(log_buf, log_buf + consumed, log_len - consumed);
  log_len -= consumed;
  return (copy > 0U) ? copy : 1U;   // 1 for an empty line, so callers keep draining
}
