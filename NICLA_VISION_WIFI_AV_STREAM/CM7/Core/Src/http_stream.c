/*
 * Tiny HTTP server for the camera (lwIP raw TCP API, port 80)
 *
 * Each connection reads one request line. "/" returns a static page,
 * "/stream" switches the connection to an endless multipart MJPEG response,
 * "/snapshot.jpg" waits for the next frame and returns it, and "/audio" is
 * a WebSocket that carries the microphone as 16 kHz 16-bit PCM (binary
 * messages of 20 ms each), played by the page's JavaScript.
 *
 * Frames are queued with TCP_WRITE_FLAG_COPY as far as the send buffer
 * allows; the rest follows from the sent/poll callbacks. A new frame is only
 * accepted once every client has queued the current one, so a slow network
 * lowers the frame rate instead of building up delay.
 */
#include "http_stream.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "main.h"
#include "lwip/tcp.h"
#include "audio.h"
#include "ws_util.h"

#define HTTP_PORT           80
#define MAX_CLIENTS         5        /* e.g. 2 pages, each with video + audio */
#define REQ_MAX             1024     /* WebSocket requests carry all headers */
#define REQ_TIMEOUT_MS      5000U    /* request line must arrive within this */
#define STALL_TIMEOUT_MS    10000U   /* drop a client that stops taking data */

typedef enum
{
  C_FREE = 0,
  C_REQUEST,      /* reading the request */
  C_STREAM,       /* MJPEG: sends every frame */
  C_SNAPSHOT,     /* waits for one frame, then closes */
  C_AUDIO,        /* WebSocket: receives every audio block */
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
  "button{font:inherit;color:#eee;background:#333;border:1px solid #555;border-radius:6px;"
  "padding:6px 14px;cursor:pointer}"
  "</style></head><body>"
  "<img src=\"/stream\" alt=\"camera stream\">"
  "<div><button id=\"a\">Audio on</button> &middot; Nicla Vision &middot; 320&times;240 MJPEG"
  " + 16 kHz audio &middot; "
  "<a href=\"/snapshot.jpg\">snapshot</a> &middot; <a href=\"/stream\">raw stream</a></div>"
  /*
   * Audio player. Browsers only allow sound after a click, hence the button.
   *
   * The page resamples the 16 kHz PCM to the output rate itself (linear
   * interpolation that carries its phase and last sample across messages)
   * and schedules each AudioBuffer at an exact integer sample position right
   * after the previous one. Letting the browser resample every 20 ms block
   * separately, at fractional start times, left a one-sample glitch at each
   * block boundary: 50 clicks per second, heard as static.
   *
   * Playback starts 200 ms ahead with a 10 ms fade-in; a late message
   * re-buffers once by 150 ms, and a message that would put the schedule
   * more than 500 ms ahead is dropped. AudioWorklet would need HTTPS; this
   * works on plain HTTP.
   */
  "<script>"
  "var b=document.getElementById('a'),ctx=null,ws=null,rate=0,next=0,ph=0,last=0,fade=1;"
  "function stop(){if(ws){ws.onclose=null;ws.close();ws=null;}"
  "if(ctx){ctx.close();ctx=null;}b.textContent='Audio on';}"
  "function play(s){var n=s.length,R=16000/rate,o=new Float32Array(Math.ceil((n-ph)/R)+1),k=0;"
  "while(ph<n){var i=Math.floor(ph),f=ph-i,p=i?s[i-1]:last,c=s[i];o[k++]=(p+(c-p)*f)/32768;ph+=R;}"
  "ph-=n;last=s[n-1];if(!k)return;"
  "var now=Math.round(ctx.currentTime*rate);"
  "if(next===0){next=now+Math.round(0.2*rate);fade=1;}"
  "else if(next<now){next=now+Math.round(0.15*rate);fade=1;}"
  "else if(next>now+0.5*rate)return;"
  "if(fade){var L=Math.min(k,Math.round(0.01*rate));for(var j=0;j<L;j++)o[j]*=j/L;fade=0;}"
  "var buf=ctx.createBuffer(1,k,rate);buf.getChannelData(0).set(o.subarray(0,k));"
  "var src=ctx.createBufferSource();src.buffer=buf;src.connect(ctx.destination);"
  "src.start(next/rate);next+=k;}"
  "b.onclick=function(){if(ctx){stop();return;}"
  "ctx=new(window.AudioContext||window.webkitAudioContext)();rate=ctx.sampleRate;"
  "next=0;ph=0;last=0;fade=1;"
  "ws=new WebSocket('ws://'+location.host+'/audio');ws.binaryType='arraybuffer';"
  "ws.onmessage=function(e){if(ctx)play(new Int16Array(e.data));};"
  "ws.onclose=stop;b.textContent='Audio off';};"
  "</script>"
  "</body></html>";

static const char WS_BAD_REQUEST[] =
  "HTTP/1.1 400 Bad Request\r\n"
  "Content-Type: text/plain\r\n"
  "Connection: close\r\n"
  "\r\n"
  "WebSocket handshake expected\n";

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

static int is_audio_request(const char *req)
{
  return strncmp(req, "GET /audio", 10) == 0;
}

/* Find a header value (case-insensitive name) in the request; returns its length */
static size_t find_header(const char *req, const char *name, const char **value)
{
  size_t name_len = strlen(name);

  for (const char *line = strstr(req, "\r\n"); line != NULL; line = strstr(line, "\r\n"))
  {
    line += 2;
    size_t i = 0;
    while ((i < name_len) && (line[i] != '\0') &&
           (tolower((unsigned char)line[i]) == tolower((unsigned char)name[i])))
    {
      i++;
    }
    if ((i == name_len) && (line[i] == ':'))
    {
      const char *v = line + i + 1;
      while (*v == ' ')
      {
        v++;
      }
      const char *end = strstr(v, "\r\n");
      *value = v;
      return (end != NULL) ? (size_t)(end - v) : strlen(v);
    }
  }
  return 0;
}

/* Switch to WebSocket (RFC 6455 handshake) for the audio stream */
static void client_start_audio(client_t *c)
{
  struct tcp_pcb *pcb = c->pcb;
  const char *key = NULL;
  size_t key_len = find_header(c->req, "Sec-WebSocket-Key", &key);
  char accept[29];
  char resp[160];

  if ((key_len == 0U) || (ws_accept_key(key, key_len, accept) != 0))
  {
    tcp_write(pcb, WS_BAD_REQUEST, sizeof(WS_BAD_REQUEST) - 1U, 0);
    tcp_output(pcb);
    c->state = C_CLOSING;
    return;
  }

  int n = snprintf(resp, sizeof(resp),
                   "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                   "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", accept);
  tcp_write(pcb, resp, (u16_t)n, TCP_WRITE_FLAG_COPY);
  tcp_output(pcb);
  c->state = C_AUDIO;
}

/* Act on a complete request line */
static void client_handle_request(client_t *c)
{
  struct tcp_pcb *pcb = c->pcb;

  if (is_audio_request(c->req))
  {
    client_start_audio(c);
  }
  else if ((strncmp(c->req, "GET /stream", 11) == 0) || (strncmp(c->req, "GET /mjpeg", 10) == 0))
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

    /*
     * The first line ("GET /path HTTP/1.1") is all we need, except for the
     * WebSocket, whose key is in a header further down.
     */
    int complete = is_audio_request(c->req) ? (strstr(c->req, "\r\n\r\n") != NULL)
                                            : (strstr(c->req, "\r\n") != NULL);
    if (complete || (c->req_len >= (sizeof(c->req) - 1U)))
    {
      client_handle_request(c);
    }
  }
  else if (c->state == C_AUDIO)
  {
    /* The page never sends data; a close frame (opcode 8) ends the stream */
    uint8_t first = pbuf_get_at(p, 0);
    if ((first & 0x0FU) == 0x08U)
    {
      pbuf_free(p);
      return client_close(c);
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

  if ((c->sending || (c->state == C_CLOSING) || (c->state == C_AUDIO)) &&
      ((now - c->progress_tick) > STALL_TIMEOUT_MS))
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

int http_stream_audio_clients(void)
{
  int n = 0;
  for (int i = 0; i < MAX_CLIENTS; i++)
  {
    if (clients[i].state == C_AUDIO)
    {
      n++;
    }
  }
  return n;
}

/* Audio messages sent / skipped since the last http_stream_take_audio_stats() */
static uint32_t audio_msgs_sent;
static uint32_t audio_msgs_skipped;

/* At most this many 20 ms blocks per WebSocket message (100 ms) */
#define AUDIO_MAX_BLOCKS_PER_MSG    5U

/*
 * Send the complete 20 ms blocks from the microphone FIFO to all WebSocket
 * listeners. Normally that is one block per message; when blocks have piled
 * up (e.g. while a new TCP connection is still ramping up), up to
 * AUDIO_MAX_BLOCKS_PER_MSG go into one message, so fewer, larger segments are
 * in flight. A listener whose send buffer is full misses the message (a short
 * gap) rather than delaying the others.
 */
void http_stream_audio_pump(void)
{
  /* 4-byte WebSocket header followed by the PCM samples, sent with one tcp_write */
  static uint8_t msg[4U + (AUDIO_MAX_BLOCKS_PER_MSG * AUDIO_BLOCK_SAMPLES * 2U)] __attribute__((aligned(4)));

  while (audio_available() >= AUDIO_BLOCK_SAMPLES)
  {
    uint32_t blocks = audio_available() / AUDIO_BLOCK_SAMPLES;
    if (blocks > AUDIO_MAX_BLOCKS_PER_MSG)
    {
      blocks = AUDIO_MAX_BLOCKS_PER_MSG;
    }
    uint32_t samples = audio_read((int16_t *)(void *)(msg + 4), blocks * AUDIO_BLOCK_SAMPLES);
    u16_t payload = (u16_t)(samples * 2U);

    /* FIN + binary opcode, 126 = 16-bit extended length (server frames are unmasked) */
    msg[0] = 0x82;
    msg[1] = 126;
    msg[2] = (uint8_t)(payload >> 8);
    msg[3] = (uint8_t)payload;
    u16_t len = (u16_t)(4U + payload);

    for (int i = 0; i < MAX_CLIENTS; i++)
    {
      client_t *c = &clients[i];
      if (c->state != C_AUDIO)
      {
        continue;
      }
      if ((tcp_sndbuf(c->pcb) < len) || (tcp_sndqueuelen(c->pcb) >= (TCP_SND_QUEUELEN - 2)) ||
          (tcp_write(c->pcb, msg, len, TCP_WRITE_FLAG_COPY) != ERR_OK))
      {
        audio_msgs_skipped++;
        continue;
      }
      audio_msgs_sent++;
      c->progress_tick = HAL_GetTick();
      tcp_output(c->pcb);
    }
  }
}

void http_stream_take_audio_stats(uint32_t *sent, uint32_t *skipped)
{
  *sent = audio_msgs_sent;
  *skipped = audio_msgs_skipped;
  audio_msgs_sent = 0;
  audio_msgs_skipped = 0;
}
