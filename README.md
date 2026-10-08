# WLED Console

A small desk controller for WLED, running on an ESP32-S3 with a 1.47" LCD. It shows
up to 8 WLED presets as boxes, lets you cycle through them with the board's BOOT
button, shows brightness and WLED uptime, and draws a live "peek" of the LED strip
as a ring around the screen edge.

- Selected preset box: green. Other presets: light blue.
- Fewer than 8 presets in WLED: the spare boxes are hidden.
- WLED offline: all boxes grey, the button does nothing.
- One click on BOOT = next preset, applied to WLED immediately.

## Hardware

| Part | Detail |
|---|---|
| Board | ESP32-S3 1.47" IPS LCD dev board (Waveshare ESP32-S3-LCD-1.47 compatible) |
| Chip | ESP32-S3 (QFN56, rev v0.2), 16MB quad flash 3.3V, 8MB embedded PSRAM (not enabled in this build) |
| Display | ST7789, 172x320, SPI, used in landscape (320x172), rotation 3 (USB cable on the other side) |
| USB | Native USB-Serial/JTAG. Shows in Windows as "USB JTAG/serial debug unit" plus a COM port |
| WLED | ESP32 controller (Audio Reactive enabled), WS2812B strip, 389 LEDs |

Display pins (confirmed working): MOSI 45, SCLK 40, CS 42, DC 41, RST 39, backlight 46.
Panel config: width 172, height 320, offset_x 34, invert on, RGB order normal.
BOOT button is GPIO0 (active low).

## Why Docker?

This was developed on a managed Windows laptop that blocks unsigned executables.
`python.exe` is signed and runs, but PlatformIO's compiler (GCC `.exe` files), `pip.exe`
and the VS Code PlatformIO extension installer are blocked ("Access is denied").
So the firmware is compiled inside a Linux container, where Windows policy does not apply,
and only flashing runs on Windows (esptool is pure Python, run via the signed `python.exe`).

If your machine has no such restriction you can just use PlatformIO directly (`pio run`,
`pio run -t upload`) and skip Docker.

## Prerequisites (Windows)

1. **Docker Desktop** (or any Docker engine). Check: `docker --version`
2. **Python 3.12** with `python` on PATH. Check: `python --version`
3. A USB-C **data** cable (charge-only cables will not show a COM port).
4. A 2.4GHz WiFi network. The ESP32-S3 has no 5GHz radio.

## One-time setup

1. Put this folder somewhere permanent (the examples use `C:\Temp\wled_controller`;
   adjust paths if you use another location).

2. Create your private settings file. Copy `src\secrets.h.example` to `src\secrets.h` and edit it
   (it is git-ignored, never commit it):

   ```cpp
   #pragma once
   #define WIFI_SSID "YourNetworkName"
   #define WIFI_PASS "YourPassword"
   #define WLED_HOST "192.168.8.148"
   ```

   `WLED_HOST` is the WLED controller's address. A DHCP reservation for WLED is recommended so
   the IP does not change.

3. Optional: tune the display and behaviour in `src\config.h` (see Configuration below).

4. Create a Python environment for esptool (flashing). Use `python -m` rather than
   `pip.exe`, which is blocked on the managed laptop:

   ```
   python -m venv C:\Temp\venvtest
   C:\Temp\venvtest\Scripts\python.exe -m pip install esptool
   ```

5. Find the board's COM port: plug it in, then check Device Manager > Ports (COM & LPT)
   or run:

   ```
   Get-PnpDevice -PresentOnly | Where-Object { $_.FriendlyName -match 'COM' } | Select FriendlyName, Status
   ```

   The examples use `COM3`. Replace with yours.

6. Check the board can be reached (read-only, writes nothing):

   ```
   C:\Temp\venvtest\Scripts\python.exe -m esptool --chip esp32s3 --port COM3 flash-id
   ```

   Expect "ESP32-S3" and "Detected flash size: 16MB".

7. (Recommended before the first flash) back up the factory firmware, 16MB:

   ```
   C:\Temp\venvtest\Scripts\python.exe -m esptool --chip esp32s3 --port COM3 read-flash 0 0x1000000 C:\Temp\wled_controller\factory_backup.bin
   ```

   Restore later with `write-flash 0x0 factory_backup.bin`. Keep a copy outside the project folder.

## Build, merge, flash

Docker Desktop must be running first (check: `docker info`). Run these three scripts
from PowerShell in the project folder, in order. Each stops with a message if the step
before it failed:

```
.\build.cmd
.\merge.cmd
.\deploy.cmd
```

- **`build.cmd`**: compiles in a Docker container (first run is slow: it downloads the image,
  PlatformIO, the ESP32 toolchain and the libraries; the `pio_cache` Docker volume keeps the
  toolchain for next time). Check the output ends in `[SUCCESS]`.
- **`merge.cmd`**: deletes the old `merged.bin`, then merges bootloader, partition table and app
  into one image, and checks the new file exists.
- **`deploy.cmd`**: refuses to flash if `merged.bin` is older than `firmware.bin`, then flashes
  the board (overwrites everything on it). Press RESET afterwards. The Python path and COM port
  are set at the top of the script; edit them if yours differ. Close any serial monitor first.

If flashing cannot connect: hold BOOT, tap RESET, release BOOT, and run `deploy.cmd` again.

The scripts are plain `.cmd` files rather than PowerShell, so they are not affected by
PowerShell execution policy on the managed laptop.

> The most common mistake is flashing a stale `merged.bin` because the build or merge
> step was skipped or failed. `deploy.cmd` guards against the obvious case, but still check
> that the build ended in `[SUCCESS]`.

<details>
<summary>The underlying commands (what the scripts run)</summary>

Build:

```
docker run --rm -v C:\Temp\wled_controller:/project -v pio_cache:/root/.platformio -w /project python:3.12-slim bash -c "pip install -q platformio && pio run"
```

Merge:

```
docker run --rm -v C:\Temp\wled_controller:/project -v pio_cache:/root/.platformio -w /project python:3.12-slim bash -c "pip install -q esptool && python -m esptool --chip esp32s3 merge-bin -o merged.bin --flash-mode dio --flash-size 16MB 0x0 .pio/build/esp32s3/bootloader.bin 0x8000 .pio/build/esp32s3/partitions.bin 0xe000 /root/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin 0x10000 .pio/build/esp32s3/firmware.bin"
```

Flash:

```
C:\Temp\venvtest\Scripts\python.exe -m esptool --chip esp32s3 --port COM3 write-flash 0x0 C:\Temp\wled_controller\merged.bin
```

</details>

## Configuration

Tunable settings are in `src/config.h` (safe to commit). Rebuild, merge and flash after
changing anything.

| Setting | Default | Meaning |
|---|---|---|
| `LCD_ROTATION` | 3 | 3 = USB cable on the left edge, 1 = flipped 180 |
| `LCD_BRIGHTNESS` | 200 | 0-255 |
| `RING_THICKNESS` | 3 | Peek ring thickness in pixels |
| `RING_MODE` | `RING_FULL` | `RING_FULL` = whole border (LEDs 0-388), `RING_BOTTOM_ONLY` = bottom edge only (LEDs 0-124) |
| `ENABLE_PEEK_RING` | 1 | 0 = no WebSocket and no ring |
| `ENABLE_DOTS` | 1 | 0 = no connector animation (screen only redraws on change) |
| `DOT_SPACING_PX`, `DOT_MS_PER_PX`, `ANIM_FRAME_MS` | 14, 25, 40 | Dot gap, speed (lower = faster), redraw interval |
| `STATE_POLL_MS_CFG`, `PRESET_REFRESH_MS_CFG`, `HOLDOFF_MS_CFG`, `PEEK_FRAME_MS` | 2000, 60000, 2500, 60 | Network timings in ms |
| `COL_SELECTED`, `COL_UNSELECTED`, `COL_DOT`, `COL_OFFLINE_TEXT` | see file | Colours as `R, G, B` |

Deliberately not configurable: box positions, connector waypoints, the ring's LED ranges and
`TOTAL_LEDS`. They are tied to `bg.h` and the window layout; changing them without
regenerating the background just breaks the display.

## Project layout

| File | Purpose |
|---|---|
| `platformio.ini` | Board, framework, 16MB partitions, USB CDC flags, libraries |
| `src/main.cpp` | Everything: display, WiFi, WLED polling, WebSocket peek, button, UI |
| `src/config.h` | Tunable settings (committed) |
| `src/bg.h` | The 320x172 background image as a C array (generated, see below) |
| `src/secrets.h` | WiFi credentials and `WLED_HOST` (you create this; git-ignored, never commit it) |
| `src/secrets.h.example` | Template for `secrets.h` |
| `build.cmd`, `merge.cmd`, `deploy.cmd` | Build, merge and flash scripts |
| `.gitignore` | Ignores `.pio/`, `src/secrets.h`, `*.bak`, `*.bin` |
| `.pio/` | Build output and downloaded libraries (generated; do not commit) |

Libraries (resolved at build time, see `platformio.ini`): LovyanGFX, ArduinoJson 7,
links2004/WebSockets. Platform `espressif32` is not pinned. When this was last built it
resolved to espressif32 7.1.3 (Arduino core 2.0.17).

## How it works

- **Network task (core 0)** polls `GET /json/si` every 2 seconds for brightness, current
  preset and uptime, and reloads `GET /presets.json` every 60 seconds (names only,
  filtered while parsing). The first 8 defined preset IDs (ascending) become boxes 1-8.
- **Button** applies the next preset with `POST /json/state {"ps": id}`. The polled preset
  is ignored for 2.5 seconds afterwards so the green box doesn't flicker back.
- **Peek ring**: WLED's HTTP `/json/live` is not enabled on this controller (returns
  `{"error":4}`), so the firmware uses the WebSocket live view like the WLED app does:
  connect to `ws://<ip>/ws`, send `{"lv":true}`, and receive binary frames
  `'L', 1, R,G,B, R,G,B ...` (one RGB per LED on ESP32; every n-th LED on ESP8266). Frames
  are throttled to about 15 per second.
- **Ring mapping**, anticlockwise from bottom-left (thickness and mode set in `config.h`):
  bottom edge left to right = LEDs 0-124, right edge bottom to top = 125-194,
  top edge right to left = 195-319, left edge top to bottom = 320-388.
  In `RING_BOTTOM_ONLY` mode only the bottom edge is drawn.
  Reversed segments need no correction: the frame holds the colour of each physical LED.
- **Connector animation**: while a preset is selected, green dots flow from the PRE box down the
  connector line to the selected box (hard-coded waypoints in `drawDotsToBox`). The screen
  redraws at about 25fps in that state; offline it only redraws on change.
- **Rendering**: the whole screen is drawn into a 320x172 sprite (static background image
  first, then dynamic elements) and pushed to the display, so there is no flicker.

## Regenerating the background (`bg.h`)

`bg.h` is the mockup background converted to RGB565, **byte-swapped** (LovyanGFX's
`pushImage(const uint16_t*)` expects swapped bytes; native order shows wrong colours,
for example yellow appears blue). With Python and Pillow:

```python
from PIL import Image
im = Image.open('wled background.png').convert('RGB')
vals = []
for r, g, b in im.getdata():
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    vals.append(((v & 0xFF) << 8) | (v >> 8))   # swap bytes
# write: const uint16_t bg565[BG_W * BG_H] PROGMEM = { 0x....,0x...., ... };
```

The current `bg.h` additionally has the right end of the PRE: box shifted left by 2 pixels
(rows 20-52, columns 300-319 shifted left 2px, rightmost 2 columns filled with the dark
background). That edit was applied to the image before conversion; if you regenerate from
the original PNG you need to repeat it, or save the edited PNG as the new source.

UI element positions are measured from the mockup and live in `main.cpp`
(box x positions and widths, value text centres).

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| Screen colours wrong (yellow shows as blue) | `bg.h` not byte-swapped |
| Nothing changed after flashing | Stale `merged.bin`: rebuild, re-merge, check the timestamp |
| All boxes grey, "WLED offline" | WiFi not connected or WLED unreachable (below) |
| Never connects | Wrong SSID (SSID not found), 5GHz-only network, wrong password, or client isolation / different VLAN to WLED |
| `curl http://<wled>/json/live` gives `{"error":4}` | Normal here: HTTP live view not compiled in. The WebSocket route is used instead |
| Peek ring wrong or blank | Frame format changed (WLED version), or sample buffer too small (`PEEK_MAX` must be at least the LED count) |
| esptool "Failed to connect" | Hold BOOT, tap RESET, release BOOT; close serial monitors |
| `python -m serial.tools.miniterm` crashes with ClearCommError | Windows quirk with the S3's native USB; the chip resets and the port drops. Use on-screen status instead |
| `docker: error during connect ... dockerDesktopLinuxEngine` | Docker Desktop is not running. Start it and wait for the engine, check `docker info` |
| `pip.exe` / `gcc.exe` "Access is denied" | Managed-laptop policy blocks unsigned executables; use the Docker workflow |

## Known issues and notes

- The segment stops in WLED A-E are one LED short (WLED's `stop` is exclusive), leaving
  LEDs 62, 124, 193, 256 and 318 unassigned and dark. Segment stops should be 63, 125, 194,
  257, 319 for A-E. Segment F (stop 389) is correct.
- PSRAM is present but not enabled. Before using it, set the correct PSRAM memory type in
  `platformio.ini`; the wrong mode can stop the board booting.
- Credentials are compiled into the firmware in plain text; do not share `merged.bin`
  or `firmware.bin`. `.gitignore` excludes `*.bin`, which includes `factory_backup.bin`, so
  keep a copy of that outside the project folder.
