/*
 * WiFi settings (SSID + password) kept in internal flash
 *
 * Stored in the last 128 KB sector of flash bank 2 (0x081E0000). The CM4
 * linker script stops below it, and neither the Arduino bootloader nor the
 * upload script touches it, so the settings survive firmware updates.
 */
#ifndef WIFI_STORE_H
#define WIFI_STORE_H

#include <stdint.h>

#define WIFI_SSID_MAX   32U     /* IEEE 802.11 limit */
#define WIFI_PASS_MAX   63U     /* WPA2 passphrase limit */

typedef struct
{
  char ssid[WIFI_SSID_MAX + 1U];
  char pass[WIFI_PASS_MAX + 1U];
} wifi_credentials_t;

/* 0 and the stored network in *out, or -1 if nothing valid is stored */
int wifi_store_load(wifi_credentials_t *out);

/* Erase the sector and write the new network. 0 = OK. Takes ~1-2 s (sector erase). */
int wifi_store_save(const wifi_credentials_t *in);

/* Forget the stored network. 0 = OK. */
int wifi_store_erase(void);

#endif /* WIFI_STORE_H */
