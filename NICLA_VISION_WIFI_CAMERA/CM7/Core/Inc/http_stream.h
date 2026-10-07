/*
 * Tiny HTTP server for the camera (lwIP raw TCP API, port 80)
 *
 *   /              page showing the live stream
 *   /stream        MJPEG stream (multipart/x-mixed-replace), for browsers/VLC/OpenCV
 *   /snapshot.jpg  one JPEG frame
 *
 * All functions run from the main loop (lwIP NO_SYS).
 */
#ifndef HTTP_STREAM_H
#define HTTP_STREAM_H

#include <stdint.h>

/* Start listening. Call once after lwIP is up (after wifi_start()). */
int http_stream_init(void);

/*
 * 1 while a frame handed to http_stream_submit() is still being queued to a
 * client: the JPEG buffer must not be overwritten until this returns 0.
 */
int http_stream_busy(void);

/* 1 if at least one client is waiting for a frame */
int http_stream_wants_frame(void);

/* Send this JPEG to every waiting client. `jpeg` must stay valid while busy. */
void http_stream_submit(const uint8_t *jpeg, uint32_t len);

/* Number of connected stream/snapshot clients */
int http_stream_clients(void);

#endif /* HTTP_STREAM_H */
