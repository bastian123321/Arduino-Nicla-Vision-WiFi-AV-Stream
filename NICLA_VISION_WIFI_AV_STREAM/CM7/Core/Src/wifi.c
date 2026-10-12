/*
 * WiFi: joins the stored network, or opens a setup hotspot (see wifi.h)
 *
 *   start ── stored network? ──no──────────────────────────────┐
 *              │yes                                            v
 *            scan ── found? ──no──> HOTSPOT (scan every 60 s, join when found)
 *              │yes                    ^            │
 *              v                       │fail/timeout│found / saved via setup page
 *           JOINING ───────────────────┘<───────────┘
 *              │up
 *              v
 *          CONNECTED ── link lost > 2 min ──> HOTSPOT
 *
 * The hotspot is the CYW4343W's AP interface (192.168.4.1/24) with a DHCP
 * server and the captive portal (wifi_portal.c). It stays on while joining,
 * so the setup page can show the result, and turns off 30 s after a join.
 */
#include "wifi.h"
#include <stdio.h>
#include <string.h>
#include "main.h"
#include "net/cyw43/cyw43.h"
#include "net/cyw43/cyw43_country.h"
#include "net/dhcpserver/dhcpserver.h"
#include "cyw43_port.h"
#include "wifi_store.h"
#include "wifi_portal.h"
#include "lwip/init.h"
#include "lwip/timeouts.h"
#include "lwip/netif.h"
#include "lwip/apps/mdns.h"
#include "usb_link.h"

#define COUNTRY                 CYW43_COUNTRY_WORLDWIDE
#define JOIN_TIMEOUT_MS         20000U   /* give up on a join after this long */
#define REJOIN_INTERVAL_MS      10000U   /* while connected: retry a lost link */
#define LOST_TO_HOTSPOT_MS      120000U  /* link lost this long: open the hotspot too */
#define HOTSPOT_RETRY_MS        60000U   /* hotspot on: look for the stored network */
#define HOTSPOT_OFF_DELAY_MS    30000U   /* after a join: keep the hotspot this long */
#define SAVE_DELAY_MS           500U     /* let the setup page's answer go out first */
#define SCAN_MAX                16

/* How long other devices may cache our mDNS answers (seconds) */
#define MDNS_TTL_S              120U

typedef enum { ST_OFF, ST_BOOT_SCAN, ST_JOINING, ST_UP, ST_HOTSPOT } state_t;

static state_t st = ST_OFF;
static wifi_credentials_t saved;
static int have_saved;
static char last_error[48];
static char status_text[160];

static uint32_t join_tick;        /* last join request */
static uint32_t down_since;       /* ST_UP: when the link went away (0 = it's up) */
static uint32_t retry_tick;       /* ST_HOTSPOT: last search for the stored network */

static int ap_on;
static uint32_t ap_off_at;        /* 0 = no delayed switch-off pending */
static char ap_ssid[24];
static dhcp_server_t dhcps;

/* Scan results build up in scan_work; the page shows scan_list, the last complete scan */
static wifi_scan_entry_t scan_list[SCAN_MAX];
static int scan_n;
static wifi_scan_entry_t scan_work[SCAN_MAX];
static int scan_work_n;
static int scan_pending;          /* a scan was started and hasn't finished */

/* Deferred work requested by the setup page */
static int pending_save;
static int pending_forget;
static uint32_t pending_at;
static wifi_credentials_t pending;

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

/* ---- mDNS ----------------------------------------------------------------- */

/* TXT record of the advertised web page: tells browsers/apps where it lives */
static void http_service_txt(struct mdns_service *service, void *userdata)
{
  (void)userdata;
  mdns_resp_add_service_txtitem(service, "path=/", 6);
}

/*
 * Answer "<hostname>.local" (e.g. nicla-vision.local) on the home network and
 * advertise the web page as an _http._tcp service. lwIP probes and announces
 * the name by itself once DHCP has assigned an address, and again whenever
 * the address changes.
 */
static void mdns_start(void)
{
  struct netif *n = &cyw43_state.netif[CYW43_ITF_STA];

  mdns_resp_init();
  if (mdns_resp_add_netif(n, cyw43_port_hostname, MDNS_TTL_S) != ERR_OK)
  {
    usb_link_log("wifi: mDNS start failed");
    return;
  }
  mdns_resp_add_service(n, "Nicla Vision camera", "_http", DNSSD_PROTO_TCP, 80, MDNS_TTL_S,
                        http_service_txt, NULL);
}

/* ---- Scanning --------------------------------------------------------------- */

static int scan_result(void *env, const cyw43_ev_scan_result_t *r)
{
  (void)env;
  if ((r->ssid_len == 0U) || (r->ssid_len > 32U))
  {
    return 0;   /* hidden network */
  }

  char ssid[33];
  memcpy(ssid, r->ssid, r->ssid_len);
  ssid[r->ssid_len] = '\0';

  /* One entry per name, with the strongest signal seen */
  for (int i = 0; i < scan_work_n; i++)
  {
    if (strcmp(scan_work[i].ssid, ssid) == 0)
    {
      if (r->rssi > scan_work[i].rssi)
      {
        scan_work[i].rssi = r->rssi;
      }
      return 0;
    }
  }
  int slot = scan_work_n;
  if (scan_work_n == SCAN_MAX)
  {
    /* Full: replace the weakest if this one is stronger */
    slot = SCAN_MAX - 1;
    if (r->rssi <= scan_work[slot].rssi)
    {
      return 0;
    }
  }
  else
  {
    scan_work_n++;
  }
  strcpy(scan_work[slot].ssid, ssid);
  scan_work[slot].rssi = r->rssi;
  scan_work[slot].open = (r->auth_mode == CYW43_AUTH_OPEN) ? 1U : 0U;

  /* Keep the list sorted, strongest first */
  for (int i = slot; (i > 0) && (scan_work[i].rssi > scan_work[i - 1].rssi); i--)
  {
    wifi_scan_entry_t t = scan_work[i];
    scan_work[i] = scan_work[i - 1];
    scan_work[i - 1] = t;
  }
  return 0;
}

void wifi_scan_start(void)
{
  if (scan_pending || (st == ST_OFF))
  {
    return;
  }
  cyw43_wifi_scan_options_t opts = {0};
  scan_work_n = 0;
  if (cyw43_wifi_scan(&cyw43_state, &opts, NULL, scan_result) == 0)
  {
    scan_pending = 1;
  }
}

int wifi_scan_running(void)
{
  return scan_pending;
}

int wifi_scan_count(void)
{
  return scan_n;
}

const wifi_scan_entry_t *wifi_scan_entry(int i)
{
  return ((i >= 0) && (i < scan_n)) ? &scan_list[i] : NULL;
}

static int saved_network_in_range(void)
{
  if (!have_saved)
  {
    return 0;
  }
  for (int i = 0; i < scan_n; i++)
  {
    if (strcmp(scan_list[i].ssid, saved.ssid) == 0)
    {
      return 1;
    }
  }
  return 0;
}

/* ---- Hotspot ---------------------------------------------------------------- */

static void hotspot_start(void)
{
  if (ap_on)
  {
    ap_off_at = 0;
    return;
  }

  cyw43_wifi_ap_set_ssid(&cyw43_state, strlen(ap_ssid), (const uint8_t *)ap_ssid);
  cyw43_wifi_ap_set_password(&cyw43_state, strlen(WIFI_AP_PASSWORD), (const uint8_t *)WIFI_AP_PASSWORD);
  cyw43_wifi_ap_set_auth(&cyw43_state, CYW43_AUTH_WPA2_AES_PSK);
  cyw43_wifi_set_up(&cyw43_state, CYW43_ITF_AP, true, COUNTRY);

  /* Bringing the AP netif up made it lwIP's default route: give it back to the STA */
  netif_set_default(&cyw43_state.netif[CYW43_ITF_STA]);

  struct netif *ap = &cyw43_state.netif[CYW43_ITF_AP];
  ip_addr_t ip, nm;
  ip_addr_copy_from_ip4(ip, *netif_ip4_addr(ap));
  ip_addr_copy_from_ip4(nm, *netif_ip4_netmask(ap));
  dhcp_server_init(&dhcps, &ip, &nm);
  wifi_portal_start(netif_ip4_addr(ap));

  ap_on = 1;
  ap_off_at = 0;
  retry_tick = HAL_GetTick();
  HAL_GPIO_WritePin(LED_B_GPIO_Port, LED_B_Pin, GPIO_PIN_RESET);   /* blue on */
  usb_link_log("wifi: hotspot \"%s\" on (password %s), setup at http://%s/",
               ap_ssid, WIFI_AP_PASSWORD, ip4addr_ntoa(netif_ip4_addr(ap)));
}

static void hotspot_stop(void)
{
  if (!ap_on)
  {
    return;
  }
  wifi_portal_stop();
  dhcp_server_deinit(&dhcps);
  cyw43_wifi_set_up(&cyw43_state, CYW43_ITF_AP, false, COUNTRY);
  netif_set_default(&cyw43_state.netif[CYW43_ITF_STA]);
  ap_on = 0;
  ap_off_at = 0;
  HAL_GPIO_WritePin(LED_B_GPIO_Port, LED_B_Pin, GPIO_PIN_SET);     /* blue off */
  usb_link_log("wifi: hotspot off");
}

/* ---- Station ---------------------------------------------------------------- */

static void join_saved(void)
{
  if (!have_saved)
  {
    return;
  }
  usb_link_log("wifi: joining \"%s\"", saved.ssid);
  int ret = cyw43_wifi_join(&cyw43_state, strlen(saved.ssid), (const uint8_t *)saved.ssid,
                            strlen(saved.pass), (const uint8_t *)saved.pass,
                            (saved.pass[0] != '\0') ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN,
                            NULL, CYW43_CHANNEL_NONE);
  if (ret != 0)
  {
    usb_link_log("wifi: join request failed (%d)", ret);
  }
  join_tick = HAL_GetTick();
  st = ST_JOINING;
}

/* A join attempt failed: remember why and make sure the hotspot is on */
static void join_failed(const char *why)
{
  snprintf(last_error, sizeof(last_error), "%s", why);
  usb_link_log("wifi: \"%s\": %s", saved.ssid, why);
  cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);   /* stop the driver's own retries */
  hotspot_start();
  st = ST_HOTSPOT;
  retry_tick = HAL_GetTick();
}

/* ---- Public API --------------------------------------------------------------- */

int wifi_start(void)
{
  cyw43_port_init();
  lwip_init();
  cyw43_init(&cyw43_state);

  /* Powers the chip, downloads its firmware over SDIO and brings up the STA netif */
  cyw43_wifi_set_up(&cyw43_state, CYW43_ITF_STA, true, COUNTRY);
  if (cyw43_poll == NULL)
  {
    usb_link_log("wifi: chip did not start");
    return -1;
  }

  /*
   * "Performance" power save (the driver's default): the radio sleeps between
   * packets but wakes for every beacon (DTIM 1) and stays awake 200 ms after
   * traffic, so streaming throughput is unaffected while idle draws far less.
   */
  cyw43_wifi_pm(&cyw43_state, CYW43_PERFORMANCE_PM);
  mdns_start();

  uint8_t mac[6];
  cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac);
  snprintf(ap_ssid, sizeof(ap_ssid), "Nicla-Vision-%02X%02X", mac[4], mac[5]);
  usb_link_log("wifi: chip up, mac %02x:%02x:%02x:%02x:%02x:%02x",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  have_saved = (wifi_store_load(&saved) == 0);
  if (have_saved)
  {
    usb_link_log("wifi: stored network \"%s\", looking for it", saved.ssid);
    st = ST_BOOT_SCAN;
  }
  else
  {
    usb_link_log("wifi: no network stored yet");
    st = ST_HOTSPOT;
    hotspot_start();
  }
  wifi_scan_start();
  return 0;
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

  if (st == ST_OFF)
  {
    return;
  }
  uint32_t now = HAL_GetTick();

  /* A scan has finished: decide whether the stored network is worth joining */
  int scan_done = 0;
  if (scan_pending && !cyw43_wifi_scan_active(&cyw43_state))
  {
    scan_pending = 0;
    scan_done = 1;
    memcpy(scan_list, scan_work, sizeof(scan_list));
    scan_n = scan_work_n;
  }

  /* Setup page requests (deferred so its answer was sent before flash work) */
  if (pending_forget && ((int32_t)(now - pending_at) >= 0))
  {
    pending_forget = 0;
    wifi_store_erase();
    have_saved = 0;
    last_error[0] = '\0';
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    hotspot_start();
    st = ST_HOTSPOT;
  }
  if (pending_save && ((int32_t)(now - pending_at) >= 0))
  {
    pending_save = 0;
    if (wifi_store_save(&pending) != 0)
    {
      usb_link_log("wifi: could not store the network in flash");
    }
    saved = pending;
    have_saved = 1;
    last_error[0] = '\0';
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    join_saved();
  }

  int s = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);

  switch (st)
  {
    case ST_BOOT_SCAN:
      if (scan_done)
      {
        if (saved_network_in_range())
        {
          join_saved();
        }
        else
        {
          join_failed("network not in range");
        }
      }
      break;

    case ST_JOINING:
      if (s == CYW43_LINK_UP)
      {
        st = ST_UP;
        down_since = 0;
        last_error[0] = '\0';
        netif_set_default(&cyw43_state.netif[CYW43_ITF_STA]);
        usb_link_log("wifi: %s", wifi_status_text());
        if (ap_on)
        {
          ap_off_at = now + HOTSPOT_OFF_DELAY_MS;
        }
      }
      else if (s == CYW43_LINK_BADAUTH)
      {
        join_failed("wrong password");
      }
      else if (s == CYW43_LINK_NONET)
      {
        join_failed("network not found");
      }
      else if (s == CYW43_LINK_FAIL)
      {
        join_failed("connection failed");
      }
      else if ((now - join_tick) > JOIN_TIMEOUT_MS)
      {
        join_failed((s == CYW43_LINK_NOIP) ? "no IP address from the router" : "no answer");
      }
      break;

    case ST_UP:
      if (s == CYW43_LINK_UP)
      {
        down_since = 0;
      }
      else
      {
        if (down_since == 0U)
        {
          down_since = now;
          usb_link_log("wifi: connection lost");
        }
        if ((s != CYW43_LINK_JOIN) && (s != CYW43_LINK_NOIP) &&
            ((now - join_tick) > REJOIN_INTERVAL_MS))
        {
          join_saved();
          st = ST_UP;   /* keep counting the outage instead of the join timeout */
        }
        if ((now - down_since) > LOST_TO_HOTSPOT_MS)
        {
          join_failed("connection lost");
        }
      }
      break;

    case ST_HOTSPOT:
      if (have_saved)
      {
        if (scan_done && saved_network_in_range())
        {
          join_saved();
        }
        else if (!scan_pending && ((now - retry_tick) > HOTSPOT_RETRY_MS))
        {
          retry_tick = now;
          wifi_scan_start();
        }
      }
      break;

    default:
      break;
  }

  /* Hotspot no longer needed: switch it off a little after the join */
  if (ap_on && (ap_off_at != 0U) && ((int32_t)(now - ap_off_at) >= 0) && (st == ST_UP))
  {
    hotspot_stop();
  }
}

int wifi_set_network(const char *ssid, const char *pass)
{
  size_t sl = strlen(ssid);
  size_t pl = strlen(pass);

  if ((sl == 0U) || (sl > WIFI_SSID_MAX) || ((pl != 0U) && ((pl < 8U) || (pl > WIFI_PASS_MAX))))
  {
    return -1;
  }
  memset(&pending, 0, sizeof(pending));
  memcpy(pending.ssid, ssid, sl);
  memcpy(pending.pass, pass, pl);
  pending_save = 1;
  pending_at = HAL_GetTick() + SAVE_DELAY_MS;
  return 0;
}

void wifi_forget_network(void)
{
  pending_forget = 1;
  pending_at = HAL_GetTick() + SAVE_DELAY_MS;
}

wifi_mode_t wifi_mode(void)
{
  switch (st)
  {
    case ST_BOOT_SCAN: return WIFI_MODE_SCANNING;
    case ST_JOINING:   return ap_on ? WIFI_MODE_HOTSPOT : WIFI_MODE_CONNECTING;
    case ST_UP:        return WIFI_MODE_CONNECTED;
    case ST_HOTSPOT:   return WIFI_MODE_HOTSPOT;
    default:           return WIFI_MODE_OFF;
  }
}

int wifi_is_up(void)
{
  return (st == ST_UP) && (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP);
}

int wifi_joining(void)
{
  return (st == ST_JOINING) || (st == ST_BOOT_SCAN) || pending_save;
}

int wifi_hotspot_on(void)
{
  return ap_on;
}

const char *wifi_hotspot_ssid(void)
{
  return ap_ssid;
}

const char *wifi_saved_ssid(void)
{
  return have_saved ? saved.ssid : "";
}

const char *wifi_last_error(void)
{
  return last_error;
}

const char *wifi_hostname(void)
{
  return cyw43_port_hostname;
}

const char *wifi_ip_text(void)
{
  static char ip_text[16];
  const ip4_addr_t *ip = netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]);
  return ip4addr_ntoa_r(ip, ip_text, sizeof(ip_text));
}

const char *wifi_status_text(void)
{
  int32_t rssi = 0;

  switch (st)
  {
    case ST_OFF:
      return "off";
    case ST_BOOT_SCAN:
      snprintf(status_text, sizeof(status_text), "looking for \"%s\"", saved.ssid);
      break;
    case ST_JOINING:
      snprintf(status_text, sizeof(status_text), "connecting to \"%s\"%s", saved.ssid,
               ap_on ? " (hotspot on)" : "");
      break;
    case ST_UP:
      cyw43_wifi_get_rssi(&cyw43_state, &rssi);
      snprintf(status_text, sizeof(status_text), "up %s rssi %ld on \"%s\"%s", wifi_ip_text(),
               (long)rssi, saved.ssid, ap_on ? " (hotspot closing)" : "");
      break;
    case ST_HOTSPOT:
      snprintf(status_text, sizeof(status_text), "hotspot \"%s\" at 192.168.4.1%s%s", ap_ssid,
               (last_error[0] != '\0') ? ", last try: " : "", last_error);
      break;
  }
  return status_text;
}
