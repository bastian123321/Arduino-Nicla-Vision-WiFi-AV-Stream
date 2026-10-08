/*
 * cyw43-driver port configuration: Nicla Vision (Murata 1DX / CYW4343W on SDMMC2),
 * bare metal (no RTOS), lwIP in NO_SYS mode.
 *
 * Modelled on MicroPython's ports/stm32/cyw43_configport.h and
 * extmod/cyw43_config_common.h (MIT, Copyright (c) 2022 Damien P. George,
 * Jim Mussared), which run this same board.
 */
#ifndef CYW43_CONFIGPORT_H
#define CYW43_CONFIGPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "main.h"     /* pin labels WL_REG_ON / WL_HOST_WAKE from the .ioc */
#include "sdio.h"

/* ---- Features ---------------------------------------------------------- */
#define CYW43_USE_SPI                   (0)     /* SDIO bus */
#define CYW43_LWIP                      (1)
#define CYW43_NETUTILS                  (0)
#define CYW43_USE_STATS                 (0)
#define CYW43_ENABLE_BLUETOOTH          (0)
#define CYW43_CLEAR_SDIO_INT            (1)
#define CYW43_IOCTL_TIMEOUT_US          (1000000)

/* Paths are relative to Core/Src/net/cyw43/ */
#define CYW43_CHIPSET_FIRMWARE_INCLUDE_FILE "firmware/w4343WA1_7_45_98_50_combined.h"
#define CYW43_WIFI_NVRAM_INCLUDE_FILE   "firmware/wifi_nvram_1dx.h"

/* ---- Errors (negative values returned by the driver) -------------------- */
#define CYW43_EPERM                     (1)
#define CYW43_EIO                       (5)
#define CYW43_EINVAL                    (22)
#define CYW43_ETIMEDOUT                 (110)

/* ---- Logging: buffered, then sent as log lines by wifi_poll() ----------- */
void cyw43_port_printf(const char *fmt, ...);
#define CYW43_PRINTF(...)               cyw43_port_printf(__VA_ARGS__)

/* ---- Concurrency --------------------------------------------------------
 * Everything (driver polling, lwIP) runs from the main loop; interrupts only
 * set flags. So no locking is needed.
 */
#define CYW43_THREAD_ENTER
#define CYW43_THREAD_EXIT
#define CYW43_THREAD_LOCK_CHECK
#define CYW43_SDPCM_SEND_COMMON_WAIT    __WFI();
#define CYW43_DO_IOCTL_WAIT             __WFI();
#define CYW43_EVENT_POLL_HOOK

extern const char cyw43_port_hostname[];
#define CYW43_HOST_NAME                 cyw43_port_hostname

#define CYW43_ARRAY_SIZE(a)             (sizeof(a) / sizeof((a)[0]))

/* The driver uses MIN() (MicroPython provides it in py/misc.h) */
#ifndef MIN
#define MIN(a, b)                       (((a) < (b)) ? (a) : (b))
#endif

/* ---- Pins: (port index << 4) | pin number, e.g. PG4 = (6 << 4) | 4 ------
 * Taken from the CubeMX labels in main.h, so the .ioc is the only place
 * where they are defined.
 */
typedef uint32_t cyw43_hal_pin_obj_t;
#define CYW43_GPIO_PORT_INDEX(port)     ((((uint32_t)(port)) - GPIOA_BASE) / (GPIOB_BASE - GPIOA_BASE))
#define CYW43_LABEL_PIN(port, mask)     ((CYW43_GPIO_PORT_INDEX(port) << 4) | (uint32_t)__builtin_ctz(mask))

#define CYW43_PIN_WL_REG_ON             CYW43_LABEL_PIN(WL_REG_ON_GPIO_Port, WL_REG_ON_Pin)
#define CYW43_PIN_WL_HOST_WAKE          CYW43_LABEL_PIN(WL_HOST_WAKE_GPIO_Port, WL_HOST_WAKE_Pin)

#define CYW43_HAL_PIN_MODE_INPUT        (0)
#define CYW43_HAL_PIN_MODE_OUTPUT       (1)
#define CYW43_HAL_PIN_PULL_NONE         (0)
#define CYW43_HAL_PIN_PULL_UP           (1)
#define CYW43_HAL_PIN_PULL_DOWN         (2)

void cyw43_hal_pin_config(cyw43_hal_pin_obj_t pin, uint32_t mode, uint32_t pull, uint32_t alt);
int cyw43_hal_pin_read(cyw43_hal_pin_obj_t pin);
void cyw43_hal_pin_low(cyw43_hal_pin_obj_t pin);
void cyw43_hal_pin_high(cyw43_hal_pin_obj_t pin);
void cyw43_hal_pin_config_irq_falling(cyw43_hal_pin_obj_t pin, int enable);

/* ---- Time --------------------------------------------------------------- */
uint32_t cyw43_hal_ticks_us(void);
uint32_t cyw43_hal_ticks_ms(void);
void cyw43_delay_us(uint32_t us);
void cyw43_delay_ms(uint32_t ms);

/* ---- MAC address (derived from the STM32 unique ID) --------------------- */
#define CYW43_HAL_MAC_WLAN0             (0)
#define CYW43_HAL_MAC_BDADDR            (1)
void cyw43_hal_get_mac(int idx, uint8_t buf[6]);
void cyw43_hal_generate_laa_mac(int idx, uint8_t buf[6]);

/* ---- Polling: run from the main loop (see wifi_poll()) ------------------ */
void cyw43_schedule_internal_poll_dispatch(void (*func)(void));

/* ---- SDIO bus (sdio.c) -------------------------------------------------- */
static inline void cyw43_sdio_init(void)                    { sdio_init(); }
static inline void cyw43_sdio_reinit(void)                  { sdio_reenable(); }
static inline void cyw43_sdio_deinit(void)                  { sdio_deinit(); }
static inline void cyw43_sdio_set_irq(bool enable)          { (void)enable; }  /* WL_HOST_WAKE is used instead */
static inline void cyw43_sdio_enable_high_speed_4bit(void)  { sdio_enable_high_speed_4bit(); }

static inline int cyw43_sdio_transfer(uint32_t cmd, uint32_t arg, uint32_t *resp)
{
  return sdio_transfer(cmd, arg, resp);
}

static inline int cyw43_sdio_transfer_cmd53(uint32_t block_size, uint32_t arg, size_t len, uint8_t *buf)
{
  return sdio_transfer_cmd53(block_size, arg, len, buf);
}

#endif /* CYW43_CONFIGPORT_H */
