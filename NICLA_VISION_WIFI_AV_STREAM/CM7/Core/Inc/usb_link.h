/*
 * Framed messages to the PC over the USB CDC port (read by tools/camera_viewer.py)
 *
 * Every message is a 16-byte little-endian header followed by the payload:
 *   char[4] magic    "NVF1" = image frame, "NVT1" = text line (log message)
 *   u16     width    (0 for text)
 *   u16     height   (0 for text)
 *   u8      format   (USB_LINK_FMT_*)
 *   u8[3]   reserved
 *   u32     length   payload size in bytes
 */
#ifndef USB_LINK_H
#define USB_LINK_H

#include <stdint.h>

#define USB_LINK_FMT_TEXT     0U
#define USB_LINK_FMT_RGB565   1U
#define USB_LINK_FMT_YUV422   2U
#define USB_LINK_FMT_JPEG     3U

/* Send one frame. Returns 0 on success, -1 if no PC is reading the port. */
int usb_link_send_frame(const uint8_t *data, uint32_t len,
                        uint16_t width, uint16_t height, uint8_t format);

/* printf-style log line, shown by the viewer */
void usb_link_log(const char *fmt, ...);

#endif /* USB_LINK_H */
