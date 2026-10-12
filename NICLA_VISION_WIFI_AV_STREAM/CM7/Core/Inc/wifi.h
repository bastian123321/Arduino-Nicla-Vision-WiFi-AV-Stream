/*
 * WiFi: joins the stored network, or opens a setup hotspot
 *
 * At start-up the board scans for the network stored in flash (wifi_store.c).
 * If it is in range, it joins it (station mode). If nothing is stored, the
 * network isn't found or joining fails, it opens the hotspot
 * "Nicla-Vision-XXXX" (WPA2, password WIFI_AP_PASSWORD) at 192.168.4.1, with
 * a DHCP server and a captive portal leading to the setup page. While the
 * hotspot is on it looks for the stored network every minute and switches
 * back to it when it appears.
 */
#ifndef WIFI_H
#define WIFI_H

#include <stdint.h>

/* Password of the setup hotspot (WPA2 needs at least 8 characters) */
#define WIFI_AP_PASSWORD   "niclasetup"

typedef enum
{
  WIFI_MODE_OFF = 0,       /* chip didn't start */
  WIFI_MODE_SCANNING,      /* start-up scan for the stored network */
  WIFI_MODE_CONNECTING,    /* joining the stored network */
  WIFI_MODE_CONNECTED,     /* joined and got an IP address */
  WIFI_MODE_HOTSPOT,       /* setup hotspot on (possibly still trying to join) */
} wifi_mode_t;

typedef struct
{
  char ssid[33];
  int16_t rssi;            /* dBm */
  uint8_t open;            /* 1 = no password */
} wifi_scan_entry_t;

/* Power up the chip, load its firmware and start the sequence above. 0 = OK. */
int wifi_start(void);

/* Run the driver, lwIP timers, joins, scans and the hotspot. Call often. */
void wifi_poll(void);

wifi_mode_t wifi_mode(void);

/* 1 once joined and an IP address was assigned */
int wifi_is_up(void);

/* 1 while a join is pending or in progress */
int wifi_joining(void);

/* 1 while the setup hotspot is on */
int wifi_hotspot_on(void);
const char *wifi_hotspot_ssid(void);

/* One-line status for logs and the setup page */
const char *wifi_status_text(void);

/* Current station IPv4 address as text ("0.0.0.0" before DHCP) */
const char *wifi_ip_text(void);

/* Hostname for DHCP and mDNS: the board answers "<hostname>.local" */
const char *wifi_hostname(void);

/* Stored network ("" if none) and why the last join attempt failed ("" if it didn't) */
const char *wifi_saved_ssid(void);
const char *wifi_last_error(void);

/* Networks seen by the last scan, strongest first */
int wifi_scan_count(void);
const wifi_scan_entry_t *wifi_scan_entry(int i);
int wifi_scan_running(void);
void wifi_scan_start(void);

/*
 * From the setup page: store this network and join it (the hotspot stays on
 * until the join succeeds). Returns 0, or -1 if the values are invalid.
 */
int wifi_set_network(const char *ssid, const char *pass);

/* From the setup page: forget the stored network and stay in hotspot mode */
void wifi_forget_network(void);

#endif /* WIFI_H */
