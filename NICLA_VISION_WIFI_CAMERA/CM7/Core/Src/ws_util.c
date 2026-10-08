/*
 * WebSocket handshake helper (RFC 6455): Sec-WebSocket-Accept from the key
 *
 * Contains a minimal SHA-1 (FIPS 180-1) for this one purpose. SHA-1 is only
 * used as the handshake checksum the protocol prescribes, not for security.
 */
#include "ws_util.h"
#include <stdint.h>
#include <string.h>

#define ROL(x, n)   (((x) << (n)) | ((x) >> (32U - (n))))

static void sha1_block(uint32_t h[5], const uint8_t blk[64])
{
  uint32_t w[80];

  for (int i = 0; i < 16; i++)
  {
    w[i] = ((uint32_t)blk[4 * i] << 24) | ((uint32_t)blk[4 * i + 1] << 16) |
           ((uint32_t)blk[4 * i + 2] << 8) | (uint32_t)blk[4 * i + 3];
  }
  for (int i = 16; i < 80; i++)
  {
    w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1U);
  }

  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
  for (int i = 0; i < 80; i++)
  {
    uint32_t f, k;
    if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999U; }
    else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1U; }
    else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDCU; }
    else             { f = b ^ c ^ d;                    k = 0xCA62C1D6U; }
    uint32_t t = ROL(a, 5U) + f + e + k + w[i];
    e = d;
    d = c;
    c = ROL(b, 30U);
    b = a;
    a = t;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

/* SHA-1 of a short message (at most 119 bytes, enough for key + GUID) */
static int sha1_short(const uint8_t *msg, size_t len, uint8_t out[20])
{
  uint8_t buf[128];
  uint32_t h[5] = { 0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U };

  if (len > (sizeof(buf) - 9U))
  {
    return -1;
  }
  size_t total = ((len + 9U + 63U) / 64U) * 64U;   /* message + 0x80 + 64-bit length */
  memset(buf, 0, sizeof(buf));
  memcpy(buf, msg, len);
  buf[len] = 0x80;
  uint64_t bits = (uint64_t)len * 8U;
  for (int i = 0; i < 8; i++)
  {
    buf[total - 1U - (size_t)i] = (uint8_t)(bits >> (8 * i));
  }
  for (size_t off = 0; off < total; off += 64U)
  {
    sha1_block(h, buf + off);
  }
  for (int i = 0; i < 5; i++)
  {
    out[4 * i]     = (uint8_t)(h[i] >> 24);
    out[4 * i + 1] = (uint8_t)(h[i] >> 16);
    out[4 * i + 2] = (uint8_t)(h[i] >> 8);
    out[4 * i + 3] = (uint8_t)h[i];
  }
  return 0;
}

static void base64_20(const uint8_t in[20], char out[29])
{
  static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int o = 0;

  for (int i = 0; i < 18; i += 3)
  {
    uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
    out[o++] = tbl[(v >> 18) & 63U];
    out[o++] = tbl[(v >> 12) & 63U];
    out[o++] = tbl[(v >> 6) & 63U];
    out[o++] = tbl[v & 63U];
  }
  /* last 2 bytes -> 3 characters + one '=' */
  uint32_t v = ((uint32_t)in[18] << 16) | ((uint32_t)in[19] << 8);
  out[o++] = tbl[(v >> 18) & 63U];
  out[o++] = tbl[(v >> 12) & 63U];
  out[o++] = tbl[(v >> 6) & 63U];
  out[o++] = '=';
  out[o] = '\0';
}

int ws_accept_key(const char *key, size_t key_len, char out[29])
{
  static const char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  uint8_t msg[96];
  uint8_t digest[20];

  if (key_len > (sizeof(msg) - (sizeof(GUID) - 1U)))
  {
    return -1;
  }
  memcpy(msg, key, key_len);
  memcpy(msg + key_len, GUID, sizeof(GUID) - 1U);
  if (sha1_short(msg, key_len + sizeof(GUID) - 1U, digest) != 0)
  {
    return -1;
  }
  base64_20(digest, out);
  return 0;
}
