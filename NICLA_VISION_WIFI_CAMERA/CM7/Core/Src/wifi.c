/*
 * WiFi station: CYW4343W + lwIP, joins the network from wifi_secrets.h
 */
#include "wifi.h"
#include <stdio.h>
#include <string.h>
#include "main.h"
#include "net/cyw43/cyw43.h"
#include "net/cyw43/cyw43_country.h"
#include "cyw43_port.h"
#include "lwip/init.h"
#include "lwip/timeouts.h"
#include "lwip/netif.h"
#include "usb_link.h"

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#error "Copy Core/Inc/wifi_secrets.example.h to Core/Inc/wifi_secrets.h and enter your WiFi name and password"
#endif

/* Try again this long after a failed or lost connection */
#define REJOIN_INTERVAL_MS    10000U

static int started;
static uint32_t join_tick;
static int last_status = CYW43_LINK_DOWN;
static char status_text[64];

/* ---- lwIP port hooks ------------------------------------------------------ */

uint32_t sys_now(void)
{
  return HAL_GetTick();
}

uint32_t lwip_port_rand(void)
{
  /* xorshift32 seeded from the unique ID and the µs timer: fine for DHCP/TCP */
  static uint32_t s;
  if (s == 0U)
  {
    s = HAL_GetUIDw0() ^ HAL_GetUIDw1() ^ HAL_GetUIDw2() ^ cyw43_hal_ticks_us() ^ 0x9E3779B9U;
  }
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}

void lwip_port_assert(const char *msg, const char *file, int line)
{
  (void)msg;
  (void)file;
  (void)line;
  /* A broken lwIP invariant: stop here, red LED on (attach a debugger to see msg) */
  __disable_irq();
  HAL_GPIO_WritePin(LED_R_GPIO_Port, LED_R_Pin, GPIO_PIN_RESET);
  while (1)
  {
  }
}

/* ---- WiFi ----------------------------------------------------------------- */

static void join(void)
{
  const char *ssid = WIFI_SSID;
  const char *pass = WIFI_PASSWORD;

  usb_link_log("wifi: joining \"%s\"", ssid);
  int ret = cyw43_wifi_join(&cyw43_state, strlen(ssid), (const uint8_t *)ssid,
                            strlen(pass), (const uint8_t *)pass,
                            (pass[0] != '\0') ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN,
                            NULL, CYW43_CHANNEL_NONE);
  if (ret != 0)
  {
    usb_link_log("wifi: join request failed (%d)", ret);
  }
  join_tick = HAL_GetTick();
}

int wifi_start(void)
{
  cyw43_port_init();
  lwip_init();
  cyw43_init(&cyw43_state);

  /* Powers the chip, downloads its firmware over SDIO and brings up the STA netif */
  cyw43_wifi_set_up(&cyw43_state, CYW43_ITF_STA, true, CYW43_COUNTRY_WORLDWIDE);
  if (cyw43_poll == NULL)
  {
    usb_link_log("wifi: chip did not start");
    return -1;
  }

  /* No power saving: lowest latency and highest throughput for streaming */
  cyw43_wifi_pm(&cyw43_state, cyw43_pm_value(CYW43_NO_POWERSAVE_MODE, 0, 0, 0, 0));

  uint8_t mac[6];
  cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac);
  usb_link_log("wifi: chip up, mac %02x:%02x:%02x:%02x:%02x:%02x",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  started = 1;
  join();
  return 0;
}

static const char *status_name(int s)
{
  switch (s)
  {
    case CYW43_LINK_DOWN:    return "down";
    case CYW43_LINK_JOIN:    return "joined";
    case CYW43_LINK_NOIP:    return "waiting for IP";
    case CYW43_LINK_UP:      return "up";
    case CYW43_LINK_FAIL:    return "failed";
    case CYW43_LINK_NONET:   return "network not found";
    case CYW43_LINK_BADAUTH: return "wrong password";
    default:                 return "?";
  }
}

void wifi_poll(void)
{
  cyw43_port_poll();
  sys_check_timeouts();

  /* Forward the driver's own messages to the USB log */
  char line[120];
  while (cyw43_port_take_log_line(line, sizeof(line)) != 0U)
  {
    if (line[0] != '\0')
    {
      usb_link_log("cyw43: %s", line);
    }
  }

  if (!started)
  {
    return;
  }

  int s = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
  if (s != last_status)
  {
    last_status = s;
    usb_link_log("wifi: %s", wifi_status_text());
  }

  /* Rejoin after a failure or when the connection dropped */
  if ((s == CYW43_LINK_FAIL) || (s == CYW43_LINK_NONET) || (s == CYW43_LINK_BADAUTH) ||
      (s == CYW43_LINK_DOWN))
  {
    if ((HAL_GetTick() - join_tick) > REJOIN_INTERVAL_MS)
    {
      join();
    }
  }
}

int wifi_is_up(void)
{
  return started && (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP);
}

const char *wifi_ip_text(void)
{
  static char ip_text[16];
  const ip4_addr_t *ip = netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]);
  return ip4addr_ntoa_r(ip, ip_text, sizeof(ip_text));
}

const char *wifi_status_text(void)
{
  if (!started)
  {
    return "off";
  }

  int s = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
  if (s == CYW43_LINK_UP)
  {
    int32_t rssi = 0;
    cyw43_wifi_get_rssi(&cyw43_state, &rssi);
    const ip4_addr_t *ip = netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]);
    snprintf(status_text, sizeof(status_text), "up %s rssi %ld", ip4addr_ntoa(ip), (long)rssi);
  }
  else
  {
    snprintf(status_text, sizeof(status_text), "%s", status_name(s));
  }
  return status_text;
}
