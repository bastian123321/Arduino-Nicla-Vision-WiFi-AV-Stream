# Arduino Nicla Vision – WiFi camera (STM32CubeIDE, bare metal)

An STM32CubeIDE (HAL) project that turns the **Arduino Nicla Vision**
(STM32H747AII6) into a WiFi camera. The board joins your WiFi network and
streams the camera as MJPEG, plus the microphone as live audio. You open it in any browser, with no app or driver
on the PC. It's flashed through the stock **Arduino bootloader over USB**, so no
ST-Link is needed.

It is built on the
[Arduino Nicla Vision STM32CubeIDE template](https://github.com/bastian123321/Arduino-Nicla-Vision-STM32CUBEIDE-Template)
(its USB CDC project). That repo explains the board bring-up, the bootloader
hand-off and the upload tooling in detail.

```
GC2145 camera --DCMI + DMA (double buffer)--> YUV422 frame (QVGA, 150 KB)
   --hardware JPEG codec--> 3-10 KB JPEG --lwIP TCP--> CYW4343W WiFi (SDIO)
   --> your router --> browser at http://nicla-vision.local/

PDM microphone --DFSDM1 + DMA--> 16 kHz 16-bit PCM --WebSocket /audio--> page player
```

* 320×240 (QVGA). Typically 15–25 fps: the camera slows down in dim light,
  and the stream adapts to the WiFi speed instead of building up delay.
* **Audio**: the on-board microphone at 16 kHz, played by the page after a
  click on **Audio on**, about 0.2 s behind the video.
* Bare metal, no RTOS: everything runs from the CM7 main loop. The CM4 is unused.
* All hardware is configured in the `.ioc`, so you can review and change it
  from CubeMX.
* Found by name: the board answers **`nicla-vision.local`** (mDNS), so you
  don't need its IP address.
* **Low power**: the CPU sleeps (`__WFI`) whenever it waits for the next frame,
  which comes to about 19% load while streaming QVGA at 21 fps (built with
  `-O2`), and WiFi uses power-save between packets. With nobody watching, the
  camera sleeps as well. WiFi stays connected.
* USB keeps working as before (upload with the 1200-baud reset, plus a debug
  viewer).

**Contents**

1. [Quick start](#1-quick-start)
2. [Using the stream](#2-using-the-stream)
3. [Pins and peripherals](#3-pins-and-peripherals)
4. [CubeMX configuration](#4-cubemx-configuration)
5. [Source files](#5-source-files)
6. [Settings you may want to change](#6-settings-you-may-want-to-change)
7. [USB debug viewer](#7-usb-debug-viewer)
8. [Troubleshooting](#8-troubleshooting)
9. [Third-party code](#9-third-party-code)

---

## 1. Quick start

You need:

* **STM32CubeIDE** (tested with 1.19.0 / CubeMX 6.15).
* The **Arduino IDE** with the *Arduino Mbed OS Nicla Boards* core installed
  (*Tools → Board → Boards Manager*). It provides `dfu-util` and the Windows USB
  driver used by the upload script.
* A Nicla Vision that still has its Arduino bootloader (double-tap reset: the
  green LED pulses).
* A **2.4 GHz** WiFi network with WPA2 (WPA2/WPA3 mixed works; WPA3-only does not).

Steps:

1. `git clone https://github.com/bastian123321/Arduino-Nicla-Vision-WiFi-Camera.git`
2. **WiFi credentials**: copy
   `NICLA_VISION_WIFI_CAMERA/CM7/Core/Inc/wifi_secrets.example.h` to
   `wifi_secrets.h` in the same folder and enter your network name and password.
   `wifi_secrets.h` is in `.gitignore`, so it never gets committed.
3. Import `NICLA_VISION_WIFI_CAMERA` into STM32CubeIDE (*File → Import → Existing
   Projects into Workspace*, tick the `_CM7` and `_CM4` projects).
4. Build both cores (**Ctrl+B**). The CM7 image is about 520 KB, mostly the WiFi
   chip firmware. The CM7 project is set to `-O2` in both the Debug and Release
   configurations (*Properties → C/C++ Build → Settings → MCU GCC Compiler →
   Optimization*). If you need to step through code with an ST-Link, switch to
   `-O0` temporarily.
5. Upload: *Run → External Tools → Upload via USB (Arduino bootloader)*. If it
   isn't in the menu yet, look under *External Tools Configurations… → Program*.
   The first time, double-tap reset first.
6. Wait about 5 seconds for the board to join the network, then open
   **`http://nicla-vision.local/`**. Type the `http://`, otherwise some browsers
   start a web search.

**By name or by IP**: `nicla-vision.local` works on Windows 10/11, macOS, iOS
and Linux. On Android it depends on the browser and version. You can always
use the IP address instead: your router's list of connected devices shows it
(hostname `nicla-vision`), or run the [USB debug viewer](#7-usb-debug-viewer),
which prints a line like
`wifi: up 192.168.1.42 rssi -57 | ... | open http://nicla-vision.local/ or http://192.168.1.42/`.

## 2. Using the stream

| URL | What you get |
|-----|--------------|
| `http://nicla-vision.local/` | Page with the live video |
| `http://nicla-vision.local/stream` | Raw MJPEG stream (`multipart/x-mixed-replace`) |
| `http://nicla-vision.local/snapshot.jpg` | One JPEG frame |
| `ws://nicla-vision.local/audio` | WebSocket: microphone as 16 kHz 16-bit mono PCM (little endian), one binary message per 20 ms (up to 100 ms when catching up) |

The IP address works in place of `nicla-vision.local` everywhere. The board
also advertises the page as an `_http._tcp` service ("Nicla Vision camera"),
so network-browser apps list it.

The raw stream also opens in **VLC** (*Media → Open Network Stream*) and in
**OpenCV**:

```python
import cv2
cap = cv2.VideoCapture("http://nicla-vision.local/stream")
while True:
    ok, frame = cap.read()
    if not ok:
        break
    cv2.imshow("Nicla", frame)
    if cv2.waitKey(1) == 27:
        break
```

Browsers only allow sound after a click, so the page has an **Audio on** button.
The page resamples the audio to the output rate itself and schedules each
block at an exact sample position (no AudioWorklet, so it works on plain HTTP).

Up to 5 connections at the same time (the video and the audio each use one per page). The **green LED** blinks while
frames are sent and is off in idle mode.

## 3. Pins and peripherals

All pins are assigned to the Cortex-M7 and locked (*Signal Pinning*) in the `.ioc`.

> CubeMX suggests default pins that are **not** what the Nicla is wired to (for
> example SDMMC2_D2 on PB3 instead of PG11, or DCMI_D3 moving to PG11). If you
> change the `.ioc`, check the pin comments in `CM7/Core/Src/stm32h7xx_hal_msp.c`
> against this table.

| Function | Signal | Pin | Notes |
|----------|--------|-----|-------|
| Camera data | DCMI_D0…D7 | PC6, PC7, PE0, PE1, PE4, PD3, PE5, PE6 | |
| Camera sync | DCMI_HSYNC / VSYNC / PIXCLK | PA4 / PG9 / PA6 | |
| Camera clock (XCLK) | TIM3_CH2 | PA7 | 12 MHz PWM, speed Very High |
| Camera control | I2C3 SCL / SDA | PA8 / PC9 | GC2145 at 0x3C (7-bit) |
| WiFi bus | SDMMC2 CK / CMD | PD6 / PD7 | AF11 |
| WiFi bus | SDMMC2 D0 / D1 / D3 | PB14 / PB15 / PB4 | AF9, pull-up |
| WiFi bus | SDMMC2 D2 | **PG11** | AF10, pull-up |
| WiFi power | `WL_REG_ON` | PG4 | output, starts low |
| WiFi interrupt | `WL_HOST_WAKE` | PD15 | EXTI falling edge, NVIC priority 5 |
| µs time base | TIM2 | – | 1 MHz free-running counter |
| Microphone clock | DFSDM1_CKOUT | PD10 | AF3, 2 MHz PDM clock |
| Microphone data | DFSDM1_DATIN2 | PE7 | AF3 |
| PMIC | I2C2 SDA / SCL | PF0 / PF1 | as in the template |
| USB HS (ULPI) | USB_OTG_HS + `USB_PHY_RST` | PA3, PA5, PB0, PB1, PB5, PB10–13, PC0, PC2_C, PC3_C + PA2 | as in the template |
| LEDs | `LED_R` / `LED_G` / `LED_B` | PE3 / PC13 / PF4 | active low |

DMA: DCMI → **DMA2 Stream 3** (peripheral to memory, word, priority high).
The firmware switches it to double-buffer mode at run time.
DFSDM1_FLT0 → **DMA1 Stream 0** (peripheral to memory, word, circular, priority high).

## 4. CubeMX configuration

Everything below is already in the `.ioc`. The list is for reference, or for
rebuilding the project from the template's
[USB CDC project](https://github.com/bastian123321/Arduino-Nicla-Vision-STM32CUBEIDE-Template#part-b--add-usb-cdc-nicla_vision_usb_cdc_hs).

**Clock** (*Clock Configuration*): HSE 25 MHz bypass, PLL1 /M 5 ×N 192 /P 2 /Q 20
→ **480 MHz** CPU, 48 MHz PLL1Q (USB and SDMMC kernel clock). HPRE /2 (240 MHz),
APB1–4 /2 (120 MHz, timers 240 MHz). Supply LDO, voltage scale 0.

**System Core → CORTEX_M7**: ICache and DCache enabled. The code does the cache
maintenance for the camera DMA buffers.

| Peripheral (Cortex-M7) | Mode / settings |
|------------------------|-----------------|
| DCMI | Slave 8 bits External Synchro; PCLK falling, VSYNC low, HSYNC low; DMA as above; DCMI global interrupt on |
| I2C3 | I2C, Standard mode 100 kHz |
| TIM3 | Channel 2 PWM Generation, PSC 0, period 19, pulse 10 (12 MHz) |
| JPEG | Activated (defaults) |
| SDMMC2 | SD 4 bits Wide bus, pins as above, NVIC off |
| TIM2 | Internal clock, PSC 239, period 4294967295, no interrupt |
| DFSDM1 | Channel 2 *PDM/SPI input from ch2 and internal clock*, clock output on; output clock divider 60 (120 MHz PCLK → 2 MHz); SPI rising edge; Filter 0: regular channel 2, continuous, software trigger, fast mode, DMA mode, FastSinc, Fosr 125, Iosr 1 (→ 16 kHz); DMA as above |
| GPIO | PG4 output `WL_REG_ON` (low); PD15 `GPIO_EXTI15` falling edge `WL_HOST_WAKE`; NVIC1: EXTI line[15:10] priority 5 |

**Project Manager → Advanced Settings**, *Do Not Generate Function Call*:

* `MX_USB_DEVICE_Init`: USB must only start after the PMIC has powered the
  PHY; `main.c` calls it at the right time.
* `MX_SDMMC2_SD_Init` (also untick *Visibility (Static)*): the generated
  function would try to initialise an **SD card**. The WiFi code only uses the
  generated `HAL_SD_MspInit()` (pins, clocks) and drives SDMMC2 itself in SDIO
  mode.

## 5. Source files

All application code is in `CM7/Core/` (user files, or `USER CODE` blocks in
generated files), so regenerating from the `.ioc` keeps it.

| File | What it does |
|------|--------------|
| `Src/main.c` (USER CODE) | Board bring-up, then the main loop: WiFi poll → idle/wake → newest frame → JPEG → HTTP + USB; status line every second; `HAL_GPIO_EXTI_Callback` |
| `Src/gc2145.c` | GC2145 sensor driver (register table from OpenMV), 180° rotation, sleep |
| `Src/camera.c` | Snapshot and continuous capture (DMA double buffer), cache maintenance, sleep/wake |
| `Src/jpeg_enc.c` | Hardware JPEG: YUYV → 16×8 MCU rows fed from the codec's GetData callback |
| `Src/http_stream.c` | HTTP server on port 80: `/`, `/stream`, `/snapshot.jpg`, `/audio` (WebSocket) and the page with the audio player |
| `Src/audio.c` | Microphone: DFSDM DMA ring, DC-removing high-pass, gain, FIFO for the WebSocket |
| `Src/ws_util.c` | WebSocket handshake (SHA-1 + Base64 of the key) |
| `Src/wifi.c` | WiFi start, WPA2 join, DHCP, mDNS (`nicla-vision.local`), power save, reconnect every 10 s, status text |
| `Src/sdio.c` | SDMMC2 as an SDIO host (polled FIFO with hardware flow control) |
| `Src/cyw43_port.c`, `Inc/cyw43_configport.h` | Glue between the WiFi driver and this board |
| `Inc/lwipopts.h`, `Inc/arch/cc.h` | lwIP configuration (NO_SYS) |
| `Src/net/cyw43/` | WiFi driver and chip firmware (third party, unmodified) |
| `Src/net/lwip/`, `Inc/lwip/`, `Inc/netif/` | lwIP 2.1.2 including its mDNS responder (third party, unmodified) |
| `Src/usb_link.c` | Framed frames and log lines over USB CDC |
| `tools/camera_viewer.py` | USB debug viewer for the PC |

**RAM** (512 KB AXI SRAM): two 150 KB frame buffers, 32 KB JPEG buffer, 16 KB audio FIFO, about
90 KB for lwIP and the WiFi driver; about 70 KB stays free for the stack.

## 6. Settings you may want to change

| Setting | Where | Default |
|---------|-------|---------|
| JPEG quality (1–100) | `JPEG_QUALITY` in `main.c` | 60 |
| Microphone gain | `AUDIO_GAIN` in `audio.c` | 8 |
| Idle delay after the last viewer leaves | `IDLE_AFTER_MS` in `main.c` | 3000 ms |
| Mirror / flip | `gc2145_set_orientation()` call in `camera.c` | upright |
| Hostname (router list and `<name>.local`) | `cyw43_port_hostname` in `cyw43_port.c` | `nicla-vision` |
| WiFi power save | `cyw43_wifi_pm()` call in `wifi.c` | `CYW43_PERFORMANCE_PM` |
| SDIO bus clock | `SDIO_BUS_HZ` in `sdio.c` | 24 MHz (the chip allows up to 50) |
| WiFi country | `CYW43_COUNTRY_WORLDWIDE` in `wifi.c` | worldwide (channels 1–11) |

## 7. USB debug viewer

The board also sends every frame and a status line once a second over the
USB COM port. To watch them:

```powershell
pip install pyserial numpy opencv-python
python NICLA_VISION_WIFI_CAMERA/tools/camera_viewer.py
```

```
board: cam=0 id=0x2145 streaming | camera 21 fps, sent 21 fps, 5800 B/frame, encode 3 ms | cpu 19% | dropped 0, jpeg errors 0, restarts 0
board: wifi: up 192.168.1.42 rssi -57 | http=0, 1 viewer(s) | open http://nicla-vision.local/ or http://192.168.1.42/
board: mic: on | peak -28 dBFS | dropped blocks 0 | 1 audio listener(s), 50 msgs sent, 0 skipped
```

`cpu` is the share of time the CPU is awake. The rest of the time it sleeps in
`__WFI` until the next interrupt.

An open viewer counts as "watching", so the camera doesn't go idle while it
runs. **Close the viewer before uploading**: the upload script needs the COM port.

## 8. Troubleshooting

| What you see | Cause / fix |
|--------------|-------------|
| Build error *Copy Core/Inc/wifi_secrets.example.h to …* | Create `wifi_secrets.h` (step 2 of the quick start). |
| `nicla-vision.local` not found, but the IP works | The device or browser doesn't resolve mDNS names (some Android versions, some company networks block multicast). Use the IP. |
| `wifi start=-1 … off` / `chip did not start` | The WiFi chip doesn't answer on SDIO. Check the SDMMC2 pins (PG11 for D2!), `WL_REG_ON` on PG4, and that `HAL_SD_MspInit` is generated. |
| `network not found` | Wrong SSID, or a 5 GHz-only network. |
| `wrong password` | Check `WIFI_PASSWORD`. |
| Stuck on `waiting for IP` | Joined, but no DHCP answer. Check the router. |
| `cam=-1 id=0x0000` (red LED) | The camera doesn't answer on I2C3: check PA8/PC9 and that XCLK (TIM3_CH2 on PA7) runs. |
| Colours swapped (blue skin) | Cb/Cr order: see `CB_OFFSET` / `CR_OFFSET` in `jpeg_enc.c` (tied to `GC2145_ROTATE`). |
| `dropped` keeps rising | Encoding started too late and the camera overwrote the buffer. Something in the main loop is blocking for more than one frame period. |
| No sound after **Audio on** | Turn the volume up and check the `mic:` status line: `peak` should change when you talk. Reload the page with Ctrl+F5 after a firmware update so the browser gets the new player. |
| Upload: *Cannot open DFU device … LIBUSB_ERROR_NOT_SUPPORTED* | Another DFU-capable USB device is connected (e.g. a USB audio interface). The upload script ignores it; if your copy is older, update `tools/upload.ps1`. |
| Page loads, no video | All 5 connections are in use, or the board went idle and a stale tab is open: reload. |

## 9. Third-party code

| Component | Source | License |
|-----------|--------|---------|
| cyw43-driver | [georgerobotics/cyw43-driver](https://github.com/georgerobotics/cyw43-driver), commit in `net/cyw43/VERSION.txt` | MIT (`net/cyw43/LICENSE`) |
| CYW4343W firmware + CLM blob (`net/cyw43/firmware/w4343WA1_7_45_98_50_combined.h`), Murata 1DX NVRAM | Infineon/Cypress firmware, as distributed with cyw43-driver | See the cyw43-driver repository for its terms |
| lwIP 2.1.2 | STM32Cube FW_H7 package (`Middlewares/Third_Party/LwIP`) | BSD (`net/lwip/COPYING`) |
| GC2145 register setup | [OpenMV](https://github.com/openmv/openmv) `drivers/sensors/gc2145.c` | MIT (header of `gc2145.c`) |
| SDIO register sequences, cyw43 port structure | [MicroPython](https://github.com/micropython/micropython) `ports/stm32` | MIT (credited in `sdio.c`, `cyw43_configport.h`) |
| STM32 HAL, CMSIS, USB device library, JPEG utilities | STMicroelectronics | see the `LICENSE.txt` files in `Drivers/`, `Middlewares/`, `Utilities/` |
