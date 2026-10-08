/*
 * Framed messages to the PC over the USB CDC port (read by tools/camera_viewer.py)
 */
#include "usb_link.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "usbd_cdc_if.h"

extern USBD_HandleTypeDef hUsbDeviceHS;

/* Largest piece handed to the CDC class at once (CDC_Transmit_HS takes a u16) */
#define USB_CHUNK_SIZE      32768U

/* Before a message: give up quickly when nobody reads (port closed / no viewer) */
#define USB_START_TIMEOUT_MS    200U

/*
 * Inside a message: wait much longer. Abandoning a message halfway leaves the
 * PC with half a frame glued to the start of the next one.
 */
#define USB_CHUNK_TIMEOUT_MS    2000U

typedef struct __attribute__((packed))
{
  char     magic[4];
  uint16_t width;
  uint16_t height;
  uint8_t  format;
  uint8_t  reserved[3];
  uint32_t length;
} usb_link_header_t;

static int wait_tx_idle(uint32_t timeout_ms)
{
  USBD_CDC_HandleTypeDef *hcdc = (USBD_CDC_HandleTypeDef *)hUsbDeviceHS.pClassData;
  uint32_t start = HAL_GetTick();

  if ((hcdc == NULL) || (hUsbDeviceHS.dev_state != USBD_STATE_CONFIGURED))
  {
    return -1;
  }
  while (hcdc->TxState != 0U)
  {
    if ((HAL_GetTick() - start) > timeout_ms)
    {
      return -1;
    }
  }
  return 0;
}

/* Blocking write; the buffer may be reused as soon as this returns */
static int usb_write(const uint8_t *data, uint32_t len)
{
  while (len > 0U)
  {
    uint16_t n = (len > USB_CHUNK_SIZE) ? USB_CHUNK_SIZE : (uint16_t)len;

    if (wait_tx_idle(USB_CHUNK_TIMEOUT_MS) != 0)
    {
      return -1;
    }
    if (CDC_Transmit_HS((uint8_t *)data, n) != USBD_OK)
    {
      return -1;
    }
    data += n;
    len -= n;
  }
  return wait_tx_idle(USB_CHUNK_TIMEOUT_MS);
}

static int send_message(const char *magic, const uint8_t *data, uint32_t len,
                        uint16_t width, uint16_t height, uint8_t format)
{
  static usb_link_header_t hdr;

  memcpy(hdr.magic, magic, 4);
  hdr.width = width;
  hdr.height = height;
  hdr.format = format;
  memset(hdr.reserved, 0, sizeof(hdr.reserved));
  hdr.length = len;

  /*
   * Only start a message when the PC is actually reading. Once a wait has
   * timed out, don't wait again until the PC reads something: otherwise every
   * message would stall the main loop while no viewer is open.
   */
  static int pc_absent;
  if (wait_tx_idle(pc_absent ? 0U : USB_START_TIMEOUT_MS) != 0)
  {
    pc_absent = 1;
    return -1;
  }
  pc_absent = 0;
  if (usb_write((const uint8_t *)&hdr, sizeof(hdr)) != 0)
  {
    return -1;
  }
  return usb_write(data, len);
}

int usb_link_send_frame(const uint8_t *data, uint32_t len,
                        uint16_t width, uint16_t height, uint8_t format)
{
  return send_message("NVF1", data, len, width, height, format);
}

void usb_link_log(const char *fmt, ...)
{
  static char line[256];
  va_list args;

  va_start(args, fmt);
  int n = vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);

  if (n < 0)
  {
    return;
  }
  if ((uint32_t)n >= sizeof(line))
  {
    n = sizeof(line) - 1;
  }
  send_message("NVT1", (const uint8_t *)line, (uint32_t)n, 0, 0, USB_LINK_FMT_TEXT);
}
