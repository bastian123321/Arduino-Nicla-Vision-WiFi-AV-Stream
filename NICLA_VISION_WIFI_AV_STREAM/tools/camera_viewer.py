"""
Live view of the Nicla Vision camera over the USB CDC port.

The firmware sends framed messages (see CM7/Core/Inc/usb_link.h):
16-byte header "NVF1"/"NVT1", width, height, format, length, then the payload.

    pip install pyserial numpy opencv-python
    python tools/camera_viewer.py            # auto-detects the board's COM port
    python tools/camera_viewer.py COM7

Keys in the window:  s = swap RGB565 byte order   q / Esc = quit
Note: close the viewer before uploading (the upload script needs the COM port).
"""
import struct
import sys
import time

import cv2
import numpy as np
import serial
from serial.tools import list_ports

HEADER = struct.Struct("<4sHHB3xI")
FMT_TEXT, FMT_RGB565, FMT_YUV422, FMT_JPEG = 0, 1, 2, 3
BOARD_IDS = {(0x0483, 0x5740)}  # ST's default CDC VID:PID (see tools/upload.ps1)


def find_port():
    for p in list_ports.comports():
        if (p.vid, p.pid) in BOARD_IDS:
            return p.device
    sys.exit("Board not found - pass the COM port as an argument")


MAGICS = (b"NVF1", b"NVT1")
MAX_PAYLOAD = 4 * 1024 * 1024


class Reader:
    """Reads the serial port in large blocks and splits it into messages."""

    def __init__(self, ser):
        self.ser = ser
        self.buf = bytearray()

    def _fill(self, need):
        while len(self.buf) < need:
            chunk = self.ser.read(max(self.ser.in_waiting, 1))
            if not chunk:
                raise TimeoutError
            self.buf += chunk

    def _find_magic(self):
        hits = [i for i in (self.buf.find(m) for m in MAGICS) if i >= 0]
        return min(hits) if hits else -1

    def next_message(self):
        while True:
            # Find the next header magic, dropping anything before it
            while (i := self._find_magic()) < 0:
                del self.buf[:-3]  # keep a partial magic at the end
                self._fill(len(self.buf) + 4096)
            del self.buf[:i]

            self._fill(HEADER.size)
            magic, w, h, fmt, length = HEADER.unpack_from(self.buf)
            if length > MAX_PAYLOAD:
                del self.buf[:4]  # not a real header
                continue

            self._fill(HEADER.size + length)
            payload = bytes(self.buf[HEADER.size:HEADER.size + length])
            del self.buf[:HEADER.size + length]
            return magic, w, h, fmt, payload


def to_bgr(payload, w, h, fmt, swap):
    if fmt in (FMT_RGB565, FMT_YUV422) and len(payload) != w * h * 2:
        return None  # damaged frame
    if fmt == FMT_RGB565:
        px = np.frombuffer(payload, dtype=">u2" if not swap else "<u2").reshape(h, w)
        r = ((px >> 11) & 0x1F) << 3
        g = ((px >> 5) & 0x3F) << 2
        b = (px & 0x1F) << 3
        return np.dstack((b, g, r)).astype(np.uint8)
    if fmt == FMT_YUV422:
        yuyv = np.frombuffer(payload, dtype=np.uint8).reshape(h, w, 2)
        return cv2.cvtColor(yuyv, cv2.COLOR_YUV2BGR_YUYV)
    if fmt == FMT_JPEG:
        return cv2.imdecode(np.frombuffer(payload, dtype=np.uint8), cv2.IMREAD_COLOR)
    return None


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else find_port()
    print(f"Opening {port}")
    ser = serial.Serial(port, 115200, timeout=2)  # any baud except 1200 (= reboot to bootloader)
    if hasattr(ser, "set_buffer_size"):
        # Big driver buffer: the board keeps sending while this window redraws
        ser.set_buffer_size(rx_size=4 * 1024 * 1024)
    reader = Reader(ser)

    swap = False
    shown, t0 = 0, time.time()
    while True:
        try:
            magic, w, h, fmt, payload = reader.next_message()
        except TimeoutError:
            print("(no data - is the firmware running?)")
            continue
        length = len(payload)

        if magic == b"NVT1":
            print("board:", payload.decode(errors="replace"))
            continue

        img = to_bgr(payload, w, h, fmt, swap)
        if img is None:
            continue
        cv2.imshow("Nicla Vision", cv2.resize(img, (w * 2, h * 2), interpolation=cv2.INTER_NEAREST))
        shown += 1
        if time.time() - t0 >= 2:
            print(f"viewer: {shown / (time.time() - t0):.1f} fps, {length} bytes/frame")
            shown, t0 = 0, time.time()

        key = cv2.waitKey(1) & 0xFF
        if key in (ord("q"), 27):
            break
        if key == ord("s"):
            swap = not swap
            print("RGB565 byte order:", "little-endian" if swap else "big-endian")

    ser.close()
    cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
