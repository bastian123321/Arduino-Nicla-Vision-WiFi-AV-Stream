/*
 * WiFi settings (SSID + password) kept in internal flash (see wifi_store.h)
 *
 * The STM32H7 programs flash in 256-bit (32-byte) flash words and erases in
 * 128 KB sectors, so the record is padded to whole flash words and every save
 * erases the sector first. Erasing bank 2 doesn't stall the CM7, which runs
 * from bank 1; the CM4 (whose code is at the start of bank 2) is asleep.
 */
#include "wifi_store.h"
#include <stddef.h>
#include <string.h>
#include "main.h"

#define STORE_ADDR      0x081E0000UL        /* bank 2, sector 7 */
#define STORE_BANK      FLASH_BANK_2
#define STORE_SECTOR    FLASH_SECTOR_7
#define STORE_MAGIC     0x4E494946UL        /* "FIIN" */
#define STORE_VERSION   1U

typedef struct
{
  uint32_t magic;
  uint32_t version;
  wifi_credentials_t cred;
  uint32_t crc;                             /* CRC-32 of everything before it */
} record_t;

/* Padded to whole 32-byte flash words */
#define RECORD_WORDS    ((sizeof(record_t) + 31U) / 32U)
typedef union
{
  record_t rec;
  uint8_t raw[RECORD_WORDS * 32U];
} record_buf_t;

static uint32_t crc32(const void *data, size_t len)
{
  const uint8_t *p = data;
  uint32_t crc = 0xFFFFFFFFU;

  while (len--)
  {
    crc ^= *p++;
    for (int k = 0; k < 8; k++)
    {
      crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

int wifi_store_load(wifi_credentials_t *out)
{
  record_t rec;

  /* Flash is cached: make sure we read what was last programmed */
  SCB_InvalidateDCache_by_Addr((uint32_t *)STORE_ADDR, sizeof(record_buf_t));
  memcpy(&rec, (const void *)STORE_ADDR, sizeof(rec));

  if ((rec.magic != STORE_MAGIC) || (rec.version != STORE_VERSION) ||
      (rec.crc != crc32(&rec, offsetof(record_t, crc))))
  {
    return -1;
  }
  rec.cred.ssid[WIFI_SSID_MAX] = '\0';
  rec.cred.pass[WIFI_PASS_MAX] = '\0';
  if (rec.cred.ssid[0] == '\0')
  {
    return -1;
  }
  *out = rec.cred;
  return 0;
}

static int erase_sector(void)
{
  FLASH_EraseInitTypeDef erase = {0};
  uint32_t bad_sector = 0;

  /* Error flags left over (e.g. from the bootloader) would fail the next operation */
  __HAL_FLASH_CLEAR_FLAG_BANK2(FLASH_FLAG_ALL_ERRORS_BANK2);

  erase.TypeErase = FLASH_TYPEERASE_SECTORS;
  erase.Banks = STORE_BANK;
  erase.Sector = STORE_SECTOR;
  erase.NbSectors = 1;
  erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
  return (HAL_FLASHEx_Erase(&erase, &bad_sector) == HAL_OK) ? 0 : -1;
}

int wifi_store_save(const wifi_credentials_t *in)
{
  static record_buf_t buf __attribute__((aligned(32)));
  int ret;

  memset(&buf, 0xFF, sizeof(buf));
  memset(&buf.rec, 0, sizeof(buf.rec));
  buf.rec.magic = STORE_MAGIC;
  buf.rec.version = STORE_VERSION;
  /* The record was zeroed above, so the copies stay terminated */
  memcpy(buf.rec.cred.ssid, in->ssid, strnlen(in->ssid, WIFI_SSID_MAX));
  memcpy(buf.rec.cred.pass, in->pass, strnlen(in->pass, WIFI_PASS_MAX));
  buf.rec.crc = crc32(&buf.rec, offsetof(record_t, crc));

  HAL_FLASH_Unlock();
  ret = erase_sector();
  for (uint32_t i = 0; (ret == 0) && (i < RECORD_WORDS); i++)
  {
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD, STORE_ADDR + (i * 32U),
                          (uint32_t)&buf.raw[i * 32U]) != HAL_OK)
    {
      ret = -1;
    }
  }
  HAL_FLASH_Lock();

  SCB_InvalidateDCache_by_Addr((uint32_t *)STORE_ADDR, sizeof(buf));

  /* Read it back to be sure */
  wifi_credentials_t check;
  if ((ret == 0) && ((wifi_store_load(&check) != 0) || (strcmp(check.ssid, in->ssid) != 0)))
  {
    ret = -1;
  }
  return ret;
}

int wifi_store_erase(void)
{
  HAL_FLASH_Unlock();
  int ret = erase_sector();
  HAL_FLASH_Lock();
  SCB_InvalidateDCache_by_Addr((uint32_t *)STORE_ADDR, sizeof(record_buf_t));
  return ret;
}
