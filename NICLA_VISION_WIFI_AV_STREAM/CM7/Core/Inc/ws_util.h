/*
 * WebSocket handshake helper (RFC 6455): Sec-WebSocket-Accept from the key
 */
#ifndef WS_UTIL_H
#define WS_UTIL_H

#include <stddef.h>

/*
 * Base64(SHA-1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")) into out
 * (needs 29 bytes: 28 characters + terminator). Returns 0, or -1 if the key
 * is too long.
 */
int ws_accept_key(const char *key, size_t key_len, char out[29]);

#endif /* WS_UTIL_H */
