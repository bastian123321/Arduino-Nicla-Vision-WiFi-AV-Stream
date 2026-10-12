/*
 * Captive portal of the setup hotspot (see wifi_portal.h)
 */
#include "wifi_portal.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "wifi.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"

#define DNS_PORT        53
#define DNS_TTL_S       60U
#define DNS_MAX_MSG     512U

static struct udp_pcb *dns_pcb;
static ip4_addr_t portal_ip;
static int portal_on;
static char portal_ip_text[16];

/* ---- Captive DNS: every A query gets the hotspot address -------------------- */

static void dns_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
  (void)arg;
  static uint8_t msg[DNS_MAX_MSG + 16U];
  uint16_t len = pbuf_copy_partial(p, msg, (p->tot_len < DNS_MAX_MSG) ? p->tot_len : DNS_MAX_MSG, 0);
  pbuf_free(p);

  /* A standard query (QR = 0, opcode 0) with at least one question */
  if ((len < 12U) || ((msg[2] & 0xF8U) != 0U) || (((msg[4] << 8) | msg[5]) == 0U))
  {
    return;
  }

  /* Skip the first question's name (labels up to a 0 byte), then type + class */
  uint16_t i = 12;
  while ((i < len) && (msg[i] != 0U))
  {
    if ((msg[i] & 0xC0U) != 0U)
    {
      return;   /* compressed names don't appear in queries */
    }
    i += (uint16_t)(msg[i] + 1U);
  }
  if ((i + 5U) > len)
  {
    return;
  }
  uint16_t qtype = (uint16_t)((msg[i + 1] << 8) | msg[i + 2]);
  uint16_t end = (uint16_t)(i + 5U);

  /* Answer the first question only; other record types get an empty answer */
  msg[2] = 0x81;                      /* QR, RD */
  msg[3] = 0x80;                      /* RA, no error */
  msg[4] = 0; msg[5] = 1;             /* 1 question */
  msg[6] = 0; msg[7] = (qtype == 1U) ? 1U : 0U;
  msg[8] = msg[9] = msg[10] = msg[11] = 0;
  uint16_t out = end;
  if (qtype == 1U)
  {
    static const uint8_t answer_head[] = {
      0xC0, 0x0C,                     /* name: pointer to the question */
      0x00, 0x01, 0x00, 0x01,         /* type A, class IN */
      0x00, 0x00, (uint8_t)(DNS_TTL_S >> 8), (uint8_t)DNS_TTL_S,
      0x00, 0x04,                     /* 4-byte address */
    };
    memcpy(msg + out, answer_head, sizeof(answer_head));
    out += sizeof(answer_head);
    memcpy(msg + out, &portal_ip.addr, 4);   /* already in network order */
    out += 4U;
  }

  struct pbuf *r = pbuf_alloc(PBUF_TRANSPORT, out, PBUF_RAM);
  if (r != NULL)
  {
    pbuf_take(r, msg, out);
    udp_sendto(pcb, r, addr, port);
    pbuf_free(r);
  }
}

void wifi_portal_start(const ip4_addr_t *ap_ip)
{
  ip4_addr_copy(portal_ip, *ap_ip);
  ip4addr_ntoa_r(&portal_ip, portal_ip_text, sizeof(portal_ip_text));
  portal_on = 1;

  if (dns_pcb == NULL)
  {
    dns_pcb = udp_new_ip_type(IPADDR_TYPE_V4);
    if (dns_pcb == NULL)
    {
      return;
    }
    ip_addr_t bind_ip;
    ip_addr_copy_from_ip4(bind_ip, portal_ip);
    if (udp_bind(dns_pcb, &bind_ip, DNS_PORT) != ERR_OK)
    {
      udp_remove(dns_pcb);
      dns_pcb = NULL;
      return;
    }
    udp_recv(dns_pcb, dns_recv, NULL);
  }
}

void wifi_portal_stop(void)
{
  portal_on = 0;
  if (dns_pcb != NULL)
  {
    udp_remove(dns_pcb);
    dns_pcb = NULL;
  }
}

int wifi_portal_is_hotspot_addr(const ip_addr_t *local)
{
  return portal_on && (local != NULL) && IP_IS_V4(local) && ip4_addr_cmp(ip_2_ip4(local), &portal_ip);
}

const char *wifi_portal_ip_text(void)
{
  return portal_ip_text;
}

/* ---- Response builder ----------------------------------------------------------- */

typedef struct
{
  char *buf;
  size_t size, len;
} out_t;

static void put(out_t *o, const char *s)
{
  size_t n = strlen(s);
  if (o->len + n >= o->size)
  {
    n = (o->size > o->len + 1U) ? (o->size - o->len - 1U) : 0U;
  }
  memcpy(o->buf + o->len, s, n);
  o->len += n;
  o->buf[o->len] = '\0';
}

static void putf(out_t *o, const char *fmt, ...)
{
  char tmp[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  put(o, tmp);
}

/* Text from the outside (network names) goes through here */
static void put_html(out_t *o, const char *s)
{
  char one[2] = {0, 0};
  for (; *s != '\0'; s++)
  {
    switch (*s)
    {
      case '&':  put(o, "&amp;");  break;
      case '<':  put(o, "&lt;");   break;
      case '>':  put(o, "&gt;");   break;
      case '"':  put(o, "&quot;"); break;
      case '\'': put(o, "&#39;");  break;
      default:   one[0] = *s; put(o, one); break;
    }
  }
}

static const char STYLE[] =
  "<style>"
  "body{margin:0;background:#111;color:#ccc;font:15px system-ui,sans-serif}"
  "main{max-width:460px;margin:0 auto;padding:16px}"
  "h1{font-size:20px;color:#eee}h2{font-size:16px;color:#eee;margin-top:24px}"
  "a{color:#8ab4f8}.ok{color:#81c995}.err{color:#f28b82}"
  "ul{list-style:none;padding:0;margin:0}"
  "li button{width:100%;display:flex;justify-content:space-between;text-align:left;"
  "margin:3px 0;background:#222;border-color:#444}"
  "label{display:block;margin:10px 0 4px}"
  "input[type=text],input[type=password]{box-sizing:border-box;width:100%;font:inherit;"
  "padding:8px;color:#eee;background:#222;border:1px solid #555;border-radius:6px}"
  "button{font:inherit;color:#eee;background:#1a73e8;border:1px solid #1a73e8;border-radius:6px;"
  "padding:8px 14px;cursor:pointer;margin-top:10px}"
  ".sec{background:#333;border-color:#555}"
  "</style>";

static void page_begin(out_t *o, int status, const char *title, int refresh_s)
{
  putf(o, "HTTP/1.1 %d %s\r\n", status, (status == 200) ? "OK" : "Bad Request");
  put(o, "Content-Type: text/html; charset=utf-8\r\n"
         "Cache-Control: no-store\r\n"
         "Connection: close\r\n"
         "\r\n"
         "<!doctype html><html><head><meta charset=\"utf-8\">"
         "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
  if (refresh_s > 0)
  {
    putf(o, "<meta http-equiv=\"refresh\" content=\"%d;url=/setup\">", refresh_s);
  }
  putf(o, "<title>%s</title>", title);
  put(o, STYLE);
  putf(o, "</head><body><main><h1>%s</h1>", title);
}

static void page_end(out_t *o)
{
  put(o, "</main></body></html>");
}

/* What the board is doing right now, in words */
static void put_status(out_t *o)
{
  const char *saved = wifi_saved_ssid();
  const char *err = wifi_last_error();

  if (wifi_is_up())
  {
    put(o, "<p class=\"ok\">Connected to &quot;");
    put_html(o, saved);
    putf(o, "&quot;. On that network, open <a href=\"http://%s.local/\">http://%s.local/</a> "
            "or <a href=\"http://%s/\">http://%s/</a>.</p>",
         wifi_hostname(), wifi_hostname(), wifi_ip_text(), wifi_ip_text());
    if (wifi_hotspot_on())
    {
      put(o, "<p>This hotspot closes in a few seconds. Reconnect this device to your own WiFi.</p>");
    }
    return;
  }
  if ((wifi_mode() == WIFI_MODE_SCANNING) || wifi_joining())
  {
    put(o, "<p>Connecting to &quot;");
    put_html(o, saved);
    put(o, "&quot;&hellip; (this page reloads by itself)</p>"
           "<p>While it joins, this hotspot may drop for a moment. If it doesn't come back, "
           "reconnect to it, or join your own WiFi and open ");
    putf(o, "http://%s.local/</p>", wifi_hostname());
    return;
  }
  if (saved[0] == '\0')
  {
    put(o, "<p>No network stored yet. Pick yours below and enter its password.</p>");
  }
  else
  {
    put(o, "<p>Stored network: &quot;");
    put_html(o, saved);
    put(o, "&quot;");
    if (err[0] != '\0')
    {
      put(o, " &mdash; <span class=\"err\">last try: ");
      put_html(o, err);
      put(o, "</span>");
    }
    put(o, ". The board looks for it every minute.</p>");
  }
}

size_t wifi_portal_page(char *buf, size_t size, int refresh_s)
{
  out_t o = { buf, size, 0 };
  int count = wifi_scan_count();

  /* Keep reloading while there is something to wait for */
  if ((refresh_s <= 0) && (((count == 0) && wifi_scan_running()) ||
                           (wifi_joining() && (wifi_saved_ssid()[0] != '\0')) ||
                           (wifi_is_up() && wifi_hotspot_on())))
  {
    refresh_s = 3;
  }

  page_begin(&o, 200, "Nicla Vision WiFi setup", refresh_s);
  put_status(&o);

  put(&o, "<h2>Networks in range</h2><ul>");
  for (int i = 0; i < count; i++)
  {
    const wifi_scan_entry_t *e = wifi_scan_entry(i);
    put(&o, "<li><button type=\"button\" class=\"sec\" data-s=\"");
    put_html(&o, e->ssid);
    put(&o, "\"><span>");
    put_html(&o, e->ssid);
    putf(&o, "</span><span>%d dBm%s</span></button></li>", e->rssi, e->open ? " &middot; open" : "");
  }
  put(&o, "</ul>");
  if (count == 0)
  {
    put(&o, wifi_scan_running() ? "<p>Scanning&hellip;</p>" : "<p>No networks found.</p>");
  }
  put(&o, "<p><a href=\"/rescan\">Scan again</a></p>");

  put(&o, "<h2>Connect to</h2>"
          "<form method=\"post\" action=\"/save\">"
          "<label for=\"s\">Network name</label>"
          "<input type=\"text\" id=\"s\" name=\"ssid\" maxlength=\"32\" required "
          "autocapitalize=\"none\" autocorrect=\"off\" value=\"");
  put_html(&o, wifi_saved_ssid());
  put(&o, "\"><label for=\"p\">Password</label>"
          "<input type=\"password\" id=\"p\" name=\"pass\" maxlength=\"63\" "
          "placeholder=\"leave empty for an open network\">"
          "<label><input type=\"checkbox\" id=\"v\"> show password</label>"
          "<button>Save and connect</button></form>");

  if (wifi_saved_ssid()[0] != '\0')
  {
    put(&o, "<form method=\"post\" action=\"/forget\"><button class=\"sec\">Forget &quot;");
    put_html(&o, wifi_saved_ssid());
    put(&o, "&quot;</button></form>");
  }

  put(&o, "<h2>Camera</h2><p><a href=\"/camera\">Live video and audio</a> "
          "(also works over this hotspot)</p>"
          "<script>"
          "var s=document.getElementById('s'),p=document.getElementById('p');"
          "document.querySelectorAll('[data-s]').forEach(function(b){b.onclick=function(){"
          "s.value=b.getAttribute('data-s');p.value='';p.focus();};});"
          "document.getElementById('v').onchange=function(){p.type=this.checked?'text':'password';};"
          "</script>");
  page_end(&o);
  return o.len;
}

/* ---- Form handling -------------------------------------------------------------- */

static int hex(char c)
{
  if ((c >= '0') && (c <= '9')) return c - '0';
  if ((c >= 'a') && (c <= 'f')) return c - 'a' + 10;
  if ((c >= 'A') && (c <= 'F')) return c - 'A' + 10;
  return -1;
}

/*
 * Find `key` in an application/x-www-form-urlencoded body and decode its
 * value into out (size bytes with the terminator). 0 = OK, -1 = missing or
 * too long.
 */
static int form_field(const char *body, size_t len, const char *key, char *out, size_t size)
{
  size_t key_len = strlen(key);
  size_t i = 0;

  while (i < len)
  {
    size_t end = i;
    while ((end < len) && (body[end] != '&'))
    {
      end++;
    }
    if (((end - i) > key_len) && (memcmp(body + i, key, key_len) == 0) && (body[i + key_len] == '='))
    {
      size_t n = 0;
      for (size_t j = i + key_len + 1U; j < end; j++)
      {
        char c = body[j];
        if (c == '+')
        {
          c = ' ';
        }
        else if ((c == '%') && ((j + 2U) < end) && (hex(body[j + 1]) >= 0) && (hex(body[j + 2]) >= 0))
        {
          c = (char)((hex(body[j + 1]) << 4) | hex(body[j + 2]));
          j += 2U;
        }
        if (n + 1U >= size)
        {
          return -1;
        }
        out[n++] = c;
      }
      out[n] = '\0';
      return 0;
    }
    else if ((end - i) == key_len && (memcmp(body + i, key, key_len) == 0))
    {
      out[0] = '\0';   /* "key" without "=" */
      return 0;
    }
    i = end + 1U;
  }
  return -1;
}

static size_t message_page(char *buf, size_t size, int status, const char *html, int refresh_s)
{
  out_t o = { buf, size, 0 };
  page_begin(&o, status, "Nicla Vision WiFi setup", refresh_s);
  put(&o, html);
  put(&o, "<p><a href=\"/setup\">Back to the setup page</a></p>");
  page_end(&o);
  return o.len;
}

size_t wifi_portal_save(char *buf, size_t size, const char *body, size_t body_len)
{
  char ssid[33];
  char pass[65];
  char html[400];

  if ((form_field(body, body_len, "ssid", ssid, sizeof(ssid)) != 0) ||
      (form_field(body, body_len, "pass", pass, sizeof(pass)) != 0) ||
      (wifi_set_network(ssid, pass) != 0))
  {
    return message_page(buf, size, 400,
                        "<p class=\"err\">That didn't work: the network name needs 1&ndash;32 "
                        "characters and the password 8&ndash;63 (or none for an open network).</p>",
                        0);
  }

  out_t o = { html, sizeof(html), 0 };
  put(&o, "<p class=\"ok\">Saved. Connecting to &quot;");
  put_html(&o, ssid);
  put(&o, "&quot;&hellip;</p><p>The board remembers it, also after power-off and firmware "
          "updates. This page shows the result in a few seconds.</p>");
  return message_page(buf, size, 200, html, 5);
}

size_t wifi_portal_forget(char *buf, size_t size)
{
  wifi_forget_network();
  return message_page(buf, size, 200,
                      "<p>The stored network was erased. The board stays in hotspot mode until "
                      "you save a new one.</p>", 3);
}

size_t wifi_portal_rescan(char *buf, size_t size)
{
  wifi_scan_start();
  return message_page(buf, size, 200, "<p>Scanning for networks&hellip;</p>", 4);
}

size_t wifi_portal_redirect(char *buf, size_t size)
{
  int n = snprintf(buf, size,
                   "HTTP/1.1 302 Found\r\n"
                   "Location: http://%s/setup\r\n"
                   "Cache-Control: no-store\r\n"
                   "Content-Length: 0\r\n"
                   "Connection: close\r\n"
                   "\r\n", portal_ip_text);
  return ((n > 0) && ((size_t)n < size)) ? (size_t)n : 0U;
}
