/*
 * SDIO host on SDMMC2 for the CYW4343W WiFi chip (Nicla Vision)
 */
#ifndef SDIO_H
#define SDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void sdio_init(void);
void sdio_deinit(void);
void sdio_reenable(void);
void sdio_enable_high_speed_4bit(void);
int sdio_transfer(uint32_t cmd, uint32_t arg, uint32_t *resp);
/*
 * CMD53 with its data phase. Direction (bit 31) and block/byte mode (bit 27)
 * come from `arg`; block_size is 64 in block mode and 1 in byte mode.
 */
int sdio_transfer_cmd53(uint32_t block_size, uint32_t arg, size_t len, uint8_t *buf);

#endif /* SDIO_H */
