/*
 * WiFi station: CYW4343W + lwIP, joins the network from wifi_secrets.h
 */
#ifndef WIFI_H
#define WIFI_H

#include <stdint.h>

/* Power up the chip, load its firmware and start joining. 0 = OK. */
int wifi_start(void);

/* Run the driver, lwIP timers and reconnects. Call often from the main loop. */
void wifi_poll(void);

/* 1 once joined and an IP address was assigned */
int wifi_is_up(void);

/* One-line status for logs, e.g. "up 192.168.1.42 rssi -51" */
const char *wifi_status_text(void);

/* Current IPv4 address as text ("0.0.0.0" before DHCP) */
const char *wifi_ip_text(void);

/* Hostname for DHCP and mDNS: the board answers "<hostname>.local" */
const char *wifi_hostname(void);

#endif /* WIFI_H */
