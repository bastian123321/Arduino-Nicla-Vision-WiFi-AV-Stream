/*
 * Tiny HTTP server for the camera (lwIP raw TCP API, port 80)
 *
 * Each connection reads one request line. "/" returns a static page,
 * "/stream" switches the connection to an endless multipart MJPEG response,
 * "/snapshot.jpg" waits for the next frame and returns it.
 *
 * Frames are queued with TCP_WRITE_FLAG_COPY as far as the send buffer
 * allows; the rest follows from the sent/poll callbacks. A new frame is only
 * accepted once every client has queued the current one, so a slow network
 * lowers the frame rate instead of building up delay.
 */
#include "http_stream.h"
#include <stdio.h>
#include <string.h>
#include "main.h"
#include "lwip/tcp.h"

#define HTTP_PORT           80
#define MAX_CLIENTS         3
#define REQ_MAX             384
#define REQ_TIMEOUT_MS      5000U    /* request line must arrive within this */
#define STALL_TIMEOUT_MS    10000U   /* drop a client that stops taking data */

typedef enum
{
  C_FREE = 0,
  C_REQUEST,      /* reading the request */
  C_STREAM,       /* MJPEG: sends every frame */
  C_SNAPSHOT,     /* waits for one frame, then closes */
  C_CLOSING,      /* close once everything was sent */
} client_state_t;

typedef struct
{
  struct tcp_pcb *pcb;
  client_state_t state;
  char req[REQ_MAX];
  uint16_t req_len;

  /* Frame being queued: part header, JPEG body, trailer */
  uint8_t sending;
  char head[160];
  uint16_t head_len, head_off;
  uint32_t body_off;
  uint8_t tail_off;

  uint32_t start_tick;
  uint32_t progress_tick;
} client_t;

static client_t clients[MAX_CLIENTS];
static struct tcp_pcb *listen_pcb;
static const uint8_t *frame_data;
static uint32_t frame_len;

static const char TAIL[] = "\r\n";

static const char PAGE[] =
  "HTTP/1.1 200 OK\r\n"
  "Content-Type: text/html; charset=utf-8\r\n"
  "Cache-Control: no-cache\r\n"
  "Connection: close\r\n"
  "\r\n"
  "<!doctype html><html><head><meta charset=\"utf-8\">"
  "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
  "<title>Nicla Vision</title>"
  "<style>"
  "body{margin:0;min-height:100vh;display:flex;flex-direction:column;align-items:center;"
  "justify-content:center;gap:12px;background:#111;color:#ccc;font:14px system-ui,sans-serif}"
  "img{width:min(96vw,960px);aspect-ratio:4/3;background:#000;image-rendering:auto}"
  "a{color:#8ab4f8}"
  "</style></head><body>"
  "<img src=\"/stream\" alt=\"camera stream\">"
  "<div>Nicla Vision &middot; 320&times;240 MJPEG &middot; "
  "<a href=\"/snapshot.jpg\">snapshot</a> &middot; <a href=\"/stream\">raw stream</a></div>"
  "</body></html>";

static const char STREAM_HEADER[] =
  "HTTP/1.1 200 OK\r\n"
  "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
  "Cache-Control: no-cache, no-store, must-revalidate\r\n"
  "Pragma: no-cache\r\n"
  "Access-Control-Allow-Origin: *\r\n"
  "Connection: close\r\n"
  "\r\n";

static const char NOT_FOUND[] =
  "HTTP/1.1 404 Not Found\r\n"
  "Content-Type: text/plain\r\n"
  "Connection: close\r\n"
  "\r\n"
  "Not found. Try / or /stream\n";

/* ---- Connection helpers ------------------------------------------------- */

static void client_free(client_t *c)
{
  memset(c, 0, sizeof(*c));
}

/* Close (or abort) the connection. Returns ERR_ABRT if it had to abort. */
static err_t client_close(client_t *c)
{
  struct tcp_pcb *pcb = c->pcb;
  err_t ret = ERR_OK;

  if (pcb != NULL)
  {
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_poll(pcb, NULL, 0);
    if (tcp_close(pcb) != ERR_OK)
    {
      tcp_abort(pcb);
      ret = ERR_ABRT;
    }
  }
  client_free(c);
  return ret;
}

/* Queue as much of the current frame as the send buffer takes */
static void client_pump(client_t *c)
{
  struct tcp_pcb *pcb = c->pcb;
  int wrote = 0;

  while (c->sending)
  {
    u16_t room = tcp_sndbuf(pcb);
    if ((room == 0U) || (tcp_sndqueuelen(pcb) >= (TCP_SND_QUEUELEN - 2)))
    {
      break;
    }

    const void *ptr;
    uint32_t left;
    if (c->head_off < c->head_len)
    {
      ptr = c->head + c->head_off;
      left = c->head_len - c->head_off;
    }
    else if (c->body_off < frame_len)
    {
      ptr = frame_data + c->body_off;
      left = frame_len - c->body_off;
    }
    else
    {
      ptr = TAIL + c->tail_off;
      left = (sizeof(TAIL) - 1U) - c->tail_off;
    }

    u16_t n = (left > room) ? room : (u16_t)left;
    if (tcp_write(pcb, ptr, n, TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE) != ERR_OK)
    {
      break;  /* out of memory: retry from the sent/poll callback */
    }
    wrote = 1;

    if (c->head_off < c->head_len)
    {
      c->head_off += n;
    }
    else if (c->body_off < frame_len)
    {
      c->body_off += n;
    }
    else
    {
      c->tail_off += (uint8_t)n;
      if (c->tail_off >= (sizeof(TAIL) - 1U))
      {
        c->sending = 0;
        if (c->state == C_SNAPSHOT)
        {
          c->state = C_CLOSING;
        }
      }
    }
  }

  if (wrote)
  {
    c->progress_tick = HAL_GetTick();
    tcp_output(pcb);
  }
}

static void client_start_frame(client_t *c)
{
  int n;
  if (c->state == C_STREAM)
  {
    n = snprintf(c->head, sizeof(c->head),
                 "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %lu\r\n\r\n",
                 (unsigned long)frame_len);
  }
  else
  {
    n = snprintf(c->head, sizeof(c->head),
                 "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %lu\r\n"
                 "Cache-Control: no-cache\r\nConnection: close\r\n\r\n",
                 (unsigned long)frame_len);
  }
  c->head_len = (uint16_t)n;
  c->head_off = 0;
  c->body_off = 0;
  /* Snapshot responses end with the JPEG: skip the multipart trailer */
  c->tail_off = (c->state == C_STREAM) ? 0U : (uint8_t)(sizeof(TAIL) - 1U);
  c->sending = 1;
  c->progress_tick = HAL_GetTick();
  client_pump(c);
}

/* Act on a complete request line */
static void client_handle_request(client_t *c)
{
  struct tcp_pcb *pcb = c->pcb;

  if ((strncmp(c->req, "GET /stream", 11) == 0) || (strncmp(c->req, "GET /mjpeg", 10) == 0))
  {
    tcp_write(pcb, STREAM_HEADER, sizeof(STREAM_HEADER) - 1U, 0);
    tcp_output(pcb);
    c->state = C_STREAM;            /* frames follow from http_stream_submit() */
  }
  else if (strncmp(c->req, "GET /snapshot", 13) == 0)
  {
    c->state = C_SNAPSHOT;          /* waits for the next frame */
  }
  else if ((strncmp(c->req, "GET / ", 6) == 0) || (strncmp(c->req, "GET /index", 10) == 0))
  {
    tcp_write(pcb, PAGE, sizeof(PAGE) - 1U, 0);
    tcp_output(pcb);
    c->state = C_CLOSING;
  }
  else
  {
    tcp_write(pcb, NOT_FOUND, sizeof(NOT_FOUND) - 1U, 0);
    tcp_output(pcb);
    c->state = C_CLOSING;
  }
  c->progress_tick = HAL_GetTick();
}

/* ---- lwIP callbacks ------------------------------------------------------- */

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
  client_t *c = (client_t *)arg;

  if ((c == NULL) || (p == NULL) || (err != ERR_OK))
  {
    if (p != NULL)
    {
      pbuf_free(p);
    }
    if (c != NULL)
    {
      return client_close(c);   /* peer closed the connection */
    }
    tcp_abort(pcb);
    return ERR_ABRT;
  }

  tcp_recved(pcb, p->tot_len);

  if (c->state == C_REQUEST)
  {
    uint16_t room = (uint16_t)(sizeof(c->req) - 1U - c->req_len);
    uint16_t n = pbuf_copy_partial(p, c->req + c->req_len, (p->tot_len < room) ? p->tot_len : room, 0);
    c->req_len += n;
    c->req[c->req_len] = '\0';

    /* The first line ("GET /path HTTP/1.1") is all we need */
    if ((strstr(c->req, "\r\n") != NULL) || (c->req_len >= (sizeof(c->req) - 1U)))
    {
      client_handle_request(c);
    }
  }
  /* Anything a stream client sends later is ignored */

  pbuf_free(p);
  return ERR_OK;
}

static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
  (void)pcb;
  (void)len;
  client_t *c = (client_t *)arg;

  if (c == NULL)
  {
    return ERR_OK;
  }
  c->progress_tick = HAL_GetTick();
  client_pump(c);
  if ((c->state == C_CLOSING) && !c->sending && (tcp_sndqueuelen(c->pcb) == 0U))
  {
    return client_close(c);
  }
  return ERR_OK;
}

/* Every 500 ms per connection: retries, timeouts, delayed closes */
static err_t on_poll(void *arg, struct tcp_pcb *pcb)
{
  client_t *c = (client_t *)arg;
  uint32_t now = HAL_GetTick();

  if (c == NULL)
  {
    tcp_abort(pcb);
    return ERR_ABRT;
  }

  if ((c->state == C_REQUEST) && ((now - c->start_tick) > REQ_TIMEOUT_MS))
  {
    return client_close(c);
  }

  client_pump(c);

  if ((c->sending || (c->state == C_CLOSING)) && ((now - c->progress_tick) > STALL_TIMEOUT_MS))
  {
    tcp_abort(pcb);           /* client stopped reading */
    client_free(c);
    return ERR_ABRT;
  }
  if ((c->state == C_CLOSING) && !c->sending && (tcp_sndqueuelen(pcb) == 0U))
  {
    return client_close(c);
  }
  return ERR_OK;
}

static void on_err(void *arg, err_t err)
{
  (void)err;
  client_t *c = (client_t *)arg;

  /* The pcb is already freed by lwIP: just forget the slot */
  if (c != NULL)
  {
    client_free(c);
  }
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
  (void)arg;
  client_t *c = NULL;

  if ((err != ERR_OK) || (pcb == NULL))
  {
    return ERR_VAL;
  }
  for (int i = 0; i < MAX_CLIENTS; i++)
  {
    if (clients[i].state == C_FREE)
    {
      c = &clients[i];
      break;
    }
  }
  if (c == NULL)
  {
    tcp_abort(pcb);           /* all slots in use */
    return ERR_ABRT;
  }

  client_free(c);
  c->pcb = pcb;
  c->state = C_REQUEST;
  c->start_tick = HAL_GetTick();
  c->progress_tick = c->start_tick;

  tcp_arg(pcb, c);
  tcp_recv(pcb, on_recv);
  tcp_sent(pcb, on_sent);
  tcp_err(pcb, on_err);
  tcp_poll(pcb, on_poll, 1);
  tcp_nagle_disable(pcb);     /* send frame data right away */
  return ERR_OK;
}

/* ---- Public API ------------------------------------------------------------- */

int http_stream_init(void)
{
  struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
  if (pcb == NULL)
  {
    return -1;
  }
  if (tcp_bind(pcb, IP4_ADDR_ANY, HTTP_PORT) != ERR_OK)
  {
    tcp_close(pcb);
    return -1;
  }
  listen_pcb = tcp_listen_with_backlog(pcb, 2);
  if (listen_pcb == NULL)
  {
    return -1;
  }
  tcp_accept(listen_pcb, on_accept);
  return 0;
}

int http_stream_busy(void)
{
  for (int i = 0; i < MAX_CLIENTS; i++)
  {
    if (clients[i].sending)
    {
      return 1;
    }
  }
  return 0;
}

int http_stream_wants_frame(void)
{
  for (int i = 0; i < MAX_CLIENTS; i++)
  {
    if (((clients[i].state == C_STREAM) || (clients[i].state == C_SNAPSHOT)) && !clients[i].sending)
    {
      return 1;
    }
  }
  return 0;
}

void http_stream_submit(const uint8_t *jpeg, uint32_t len)
{
  if (http_stream_busy())
  {
    return;   /* previous frame still being queued */
  }
  frame_data = jpeg;
  frame_len = len;

  for (int i = 0; i < MAX_CLIENTS; i++)
  {
    client_t *c = &clients[i];
    if ((c->state == C_STREAM) || (c->state == C_SNAPSHOT))
    {
      client_start_frame(c);
    }
  }
}

int http_stream_clients(void)
{
  int n = 0;
  for (int i = 0; i < MAX_CLIENTS; i++)
  {
    if ((clients[i].state == C_STREAM) || (clients[i].state == C_SNAPSHOT))
    {
      n++;
    }
  }
  return n;
}
