/*
 * Captive portal of the setup hotspot
 *
 * While the hotspot is on, a tiny DNS server answers every name with the
 * board's hotspot address (192.168.4.1), and the HTTP server (http_stream.c)
 * redirects requests for other hosts to the setup page. Phones and laptops
 * see that their "is there internet?" check doesn't get the expected answer
 * and pop up the setup page by themselves.
 *
 * The setup page lists the networks in range and has a form for the network
 * name and password (POST /save), a "forget" button (POST /forget) and the
 * current status. It is also reachable on the home network at /setup.
 */
#ifndef WIFI_PORTAL_H
#define WIFI_PORTAL_H

#include <stddef.h>
#include "lwip/ip_addr.h"

/* Start / stop the captive DNS server on the hotspot address */
void wifi_portal_start(const ip4_addr_t *ap_ip);
void wifi_portal_stop(void);

/* 1 if `local` (the address a connection arrived on) is the hotspot's */
int wifi_portal_is_hotspot_addr(const ip_addr_t *local);

/* Hotspot address as text, e.g. "192.168.4.1" */
const char *wifi_portal_ip_text(void);

/*
 * Build a complete HTTP response into buf, return its length:
 *   wifi_portal_page      the setup page (refresh_s > 0: reload after that many seconds)
 *   wifi_portal_save      handles the POST /save form body
 *   wifi_portal_forget    handles POST /forget
 *   wifi_portal_rescan    starts a scan and answers with a page that reloads when it's done
 *   wifi_portal_redirect  302 to the setup page (captive-portal detection)
 */
size_t wifi_portal_page(char *buf, size_t size, int refresh_s);
size_t wifi_portal_save(char *buf, size_t size, const char *body, size_t body_len);
size_t wifi_portal_forget(char *buf, size_t size);
size_t wifi_portal_rescan(char *buf, size_t size);
size_t wifi_portal_redirect(char *buf, size_t size);

#endif /* WIFI_PORTAL_H */
