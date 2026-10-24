# WLED Console

A small desk controller for WLED, running on an ESP32-S3 with a 1.47" LCD. It shows
up to 8 WLED presets as boxes, lets you cycle through them with the board's BOOT
button, shows brightness and WLED uptime, and draws a live "peek" of the LED strip
as a ring around the screen edge.

- Selected preset box: green. Other presets: light blue. Presets that use a sound-reactive effect
  get a small equalizer icon in their box corner.
- The PRE box beside the BRI/DUR boxes shows the current preset number, and is green to match the
  selected box (grey when offline).
- Fewer than 8 presets in WLED: the spare boxes are hidden.
- WLED offline: all boxes grey, the button does nothing.
- One click on BOOT = next preset, applied to WLED immediately.
- Hold BOOT for 2 seconds = switch to **Brightness mode** (hold again to go back). The PRE box
  becomes BR: and the 8 boxes become brightness levels: the boxes step through 8 raw levels (`BRI_LEVEL_LIST`, default 1, 3, 6, 12, 25, 45, 100, 255 out of 255, i.e. about 1%, 1%, 2%, 5%, 10%, 18%, 39%, 100%).
  A click steps to the next level (the top level wraps to the lowest). The selected box (yellow, with lilac for the
  rest) is the nearest level to the current WLED brightness, and the BR: box shows the real brightness
  as a whole percent (25/255 shows as 10 and box 5; level 1 shows as 1, never 0). The dots
  turn yellow too. Offline, the button (short or long) does nothing.
- The board's onboard RGB LED (a WS2812 on GPIO38 per Waveshare's docs) slowly cycles through the
  rainbow, like the factory firmware. It switches off whenever the screen is dimmed (idle, or WLED power off).
- While the screen is dimmed (idle, or WLED power off) the live view is switched off and the ring is
  hidden; the next button press switches it back on (a fraction of a second to reappear). The WebSocket
  itself stays open, because WLED uses it to push state changes.
- After 15 seconds with no button press the backlight dims (only the backlight; polling, the peek ring
  and the animation keep running). Any button press, or holding the button, brings it back. While
  WLED power is off the backlight stays at the lower off level regardless.
- Hold BOOT for 4 seconds = turn WLED power off (sends `{"on":false}`, so the stored brightness is kept).
  From 2 seconds the bottom strip shows a hint: release to switch mode, keep holding to power off.
  While off the boxes and the PRE box are dimmed (the box just says OFF), the dots and peek ring stop, the
  backlight dims, and the bottom strip says the lights are off. A plain click turns the lights back
  on (a 4 second hold also does). The state follows WLED, so turning the lights off from the app
  shows up here within a couple of seconds.
- **Auto power-off**: once WLED has been on for 3 hours in a row (`AUTO_OFF_MS`) the controller sends
  `{"on":false}`, and retries 15 seconds later if WLED is still on. The timer is not reset by button presses;
  it restarts when WLED is seen off or unreachable, or when the controller reboots. Click the button to turn
  the lights back on. `AUTO_OFF_MS 0` disables it.

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

1. Put this folder somewhere permanent. All commands below are run from the project folder
   (the one containing `platformio.ini`) and use relative paths, so it can live anywhere.

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
   python -m venv .venv
   .venv\Scripts\python.exe -m pip install esptool
   ```

5. Find the board's COM port: plug it in, then check Device Manager > Ports (COM & LPT)
   or run:

   ```
   Get-PnpDevice -PresentOnly | Where-Object { $_.FriendlyName -match 'COM' } | Select FriendlyName, Status
   ```

   The examples use `COM3`. Replace with yours.

6. Check the board can be reached (read-only, writes nothing):

   ```
   .venv\Scripts\python.exe -m esptool --chip esp32s3 --port COM3 flash-id
   ```

   Expect "ESP32-S3" and "Detected flash size: 16MB".

7. (Recommended before the first flash) back up the factory firmware, 16MB:

   ```
   .venv\Scripts\python.exe -m esptool --chip esp32s3 --port COM3 read-flash 0 0x1000000 factory_backup.bin
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

To run all three in one go, use `.\full_deploy.cmd`. It stops at the first step that fails.

- **`build.cmd`**: compiles in a Docker container (first run is slow: it downloads the image,
  PlatformIO, the ESP32 toolchain and the libraries; the `pio_cache` Docker volume keeps the
  toolchain for next time). The libraries and build output live in a second Docker volume,
  `pio_work`, because scanning thousands of library files across the Windows bind mount made every
  build slow; the three `.bin` files are copied back to `.pio/build/esp32s3/` after a good build.
  To force a clean rebuild: `docker volume rm pio_work`. Check the output ends in `[SUCCESS]`.
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
docker run --rm -v ${PWD}:/project -v pio_cache:/root/.platformio -v pio_work:/pio_work -e PLATFORMIO_WORKSPACE_DIR=/pio_work -e PIP_CACHE_DIR=/root/.platformio/pipcache -w /project python:3.12-slim bash -c "pip install -q platformio && pio run && mkdir -p .pio/build/esp32s3 && cp /pio_work/build/esp32s3/firmware.bin /pio_work/build/esp32s3/bootloader.bin /pio_work/build/esp32s3/partitions.bin .pio/build/esp32s3/"
```

Merge:

```
docker run --rm -v ${PWD}:/project -v pio_cache:/root/.platformio -w /project python:3.12-slim bash -c "pip install -q esptool && python -m esptool --chip esp32s3 merge-bin -o merged.bin --flash-mode dio --flash-size 16MB 0x0 .pio/build/esp32s3/bootloader.bin 0x8000 .pio/build/esp32s3/partitions.bin 0xe000 /root/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin 0x10000 .pio/build/esp32s3/firmware.bin"
```

Flash:

```
.venv\Scripts\python.exe -m esptool --chip esp32s3 --port COM3 write-flash 0x0 merged.bin
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
| `PRE_LABEL_SCALE` | 1.15 | Size of the PRE: label |
| `LONG_PRESS_MS` | 2000 | Hold time (then release) to switch Preset / Brightness mode |
| `POWER_PRESS_MS` | 4000 | Hold time to toggle WLED power |
| `LCD_BRIGHTNESS_OFF` | 6 | Backlight while WLED power is off |
| `IDLE_DIM_MS` | 15000 | Dim the backlight after this long with no button press (0 = never) |
| `LCD_BRIGHTNESS_IDLE` | 10 | Backlight level while idle |
| `ENABLE_RGB_LED`, `RGB_LED_PIN`, `RGB_LED_LEVEL`, `RGB_LED_CYCLE_MS` | 1, 38, 100, 6000 | Onboard RGB LED rainbow: on/off, pin, brightness 0-255, ms per full cycle |
| `ENABLE_PEEK_RING` | 1 | 0 = no live view and no ring (the WebSocket still carries state pushes; live view is also off while the screen is dimmed) |
| `ENABLE_AUDIO_MARKER` | 1 | Equalizer icon in the corner of boxes for sound-reactive presets |
| `BRI_LEVEL_LIST` | 1, 3, 6, 12, 25, 45, 100, 255 | The 8 brightness-mode levels, raw WLED brightness 1-255 (exactly 8, ascending) |
| `LAST_PRESET_SOLID` | 1 | Show the last chosen preset as selected when WLED reports none (0 = outline) |
| `ENABLE_DOTS` | 1 | 0 = no connector animation (screen only redraws on change) |
| `DOT_SPACING_PX`, `DOT_MS_PER_PX`, `ANIM_FRAME_MS` | 14, 25, 40 | Dot gap, speed (lower = faster), redraw interval |
| `STATE_POLL_MS_CFG`, `STATE_POLL_SLOW_MS`, `PRESET_REFRESH_MS_CFG`, `HOLDOFF_MS_CFG`, `PEEK_FRAME_MS` | 2000, 10000, 60000, 2500, 60 | Network timings in ms. The state poll runs at `STATE_POLL_MS_CFG` while the WebSocket is down or a poll just failed, and at `STATE_POLL_SLOW_MS` while it is up |
| `AUTO_OFF_MS` | 10800000 (3h) | Send WLED power off after it has been on this long in a row (0 = never). Counts from when the controller first sees it on; restarts if WLED goes off or unreachable or the controller reboots; button presses don't reset it |
| `OFFLINE_AFTER_FAILS` | 3 | Failed state polls in a row before the screen shows WLED offline (one dropped request no longer flips the UI) |
| `COL_SELECTED`, `COL_UNSELECTED`, `COL_DOT`, `COL_OFFLINE_TEXT`, `COL_CAPTION`, `COL_SELECTED_BRI`, `COL_UNSELECTED_BRI`, `COL_OFF_SELECTED`, `COL_OFF_UNSELECTED`, `COL_OFF_TEXT` | see file | Colours as `R, G, B` |

Deliberately not configurable: box positions, connector waypoints, the ring's LED ranges and
`TOTAL_LEDS`. They are tied to `bg.h` and the window layout; changing them without
regenerating the background just breaks the display.

## Project layout

| File | Purpose |
|---|---|
| `platformio.ini` | Board, framework, 16MB partitions, USB CDC flags, libraries |
| `src/main.cpp` | Everything: display, WiFi, WLED polling, WebSocket state push and peek, button, UI |
| `src/config.h` | Tunable settings (committed) |
| `src/bg.h` | The 320x172 background image as a C array (generated, see below) |
| `src/secrets.h` | WiFi credentials and `WLED_HOST` (you create this; git-ignored, never commit it) |
| `src/secrets.h.example` | Template for `secrets.h` |
| `build.cmd`, `merge.cmd`, `deploy.cmd` | Build, merge and flash scripts |
| `full_deploy.cmd` | Runs build, merge and flash in order, stopping on the first failure |
| `.gitignore` | Ignores `.pio/`, `src/secrets.h`, `*.bak`, `*.bin` |
| `.pio/` | Generated; holds the three `.bin` files copied from the build (libraries and build output live in the `pio_work` Docker volume). Do not commit |

Libraries (resolved at build time, see `platformio.ini`): LovyanGFX, ArduinoJson 7,
links2004/WebSockets. Platform `espressif32` is not pinned. When this was last built it
resolved to espressif32 7.1.3 (Arduino core 2.0.17).

## How it works

- **Network task (core 0)** keeps a WebSocket open to WLED, which pushes every change to brightness,
  power (`on`) and current preset. `GET /json/si` is only a safety net and for uptime: every 10 seconds
  while the WebSocket is up, every 2 seconds when it is not. WLED is declared offline after 3 failed
  polls in a row, not one. `GET /presets.json` is reloaded every 60 seconds (names and each segment's
  effect id `fx`, filtered while parsing), and `GET /json/fxdata` is read once. Both back off (5s, 10s,
  30s, then 60s) when they fail, are skipped while WLED is unreachable, and `fxdata` is abandoned after
  3 failures (no audio markers until reboot). The DUR box counts uptime on locally between polls.
  The first 8 defined preset IDs (ascending) become boxes 1-8.
- **Remembered preset**: WLED reports `ps` as -1 or 0 whenever the live state isn't an exact saved
  preset (after a boot with no boot preset, or after manual changes). The firmware remembers the
  last preset chosen (button click, or WLED reporting one) in flash (`Preferences`, namespace
  `wledctl`) and, when WLED reports none, shows that box as selected (`LAST_PRESET_SOLID 1`, the default, so a
  brightness change doesn't look like the preset was deselected). With `LAST_PRESET_SOLID 0` it is
  drawn with a green outline and "(modified)" is added to the name instead. The next click continues
  from that box. The trade-off: if the state was changed some other way (e.g. colours in the app),
  the last preset still shows as selected.
- **Sound-reactive marker**: at boot the firmware fetches `GET /json/fxdata` (one metadata string per
  effect id, `sliders;colors;palette;flags;defaults`) and records which effects have `v` or `f` in
  the flags field (volume / frequency reactive). Each preset's segment `fx` ids are checked against
  that table, and every box whose preset has a sound-reactive effect in any segment shows a small
  equalizer icon in its top-right corner (black on the selected box, white on the others; hidden in
  Brightness mode and while the lights are off). If `fxdata` can't be read after 3 tries the markers are skipped until the next reboot. Limits: a playlist preset (no `seg`) is not
  followed, and the marker says the effect *can* react to sound, not that the AudioReactive usermod is enabled.
- **Button**: a short press is acted on at release (so a long press cannot also fire a click). It applies
  the next preset with `POST /json/state {"ps": id}`; a 4 second hold sends `{"on":true/false}`. The
  polled preset, brightness and power state are ignored for 2.5 seconds afterwards so the green box doesn't flicker back. In Brightness mode it
  sends `POST /json/state {"bri": n}` with the raw level for the box (`BRI_LEVEL_LIST`) and the same holdoff.
  The PRESET/PRE: and BRIGHT/BR: texts are drawn in code over the background, so the background
  image can have those labels removed or left in.
- **Peek ring**: WLED's HTTP `/json/live` is not enabled on this controller (returns
  `{"error":4}`), so the firmware uses the WebSocket live view like the WLED app does:
  connect to `ws://<ip>/ws`, send `{"lv":true}` (and `{"lv":false}` when the screen dims or WLED is off), and receive binary frames
  `'L', 1, R,G,B, R,G,B ...` (one RGB per LED on ESP32; every n-th LED on ESP8266). Frames
  are throttled to about 15 per second. WLED tracks one live-view client at a time, so opening the peek view in the
  WLED web UI can take it from the controller (untested); the ring comes back the next time the screen dims and wakes.
- **Ring mapping**, anticlockwise from bottom-left (thickness and mode set in `config.h`):
  bottom edge left to right = LEDs 0-124, right edge bottom to top = 125-194,
  top edge right to left = 195-319, left edge top to bottom = 320-388.
  In `RING_BOTTOM_ONLY` mode only the bottom edge is drawn.
  Reversed segments need no correction: the frame holds the colour of each physical LED.
- **Connector animation**: while a preset is selected (and the lights are on), green dots flow from the PRE box down the
  connector line to the selected box (yellow in Brightness mode; hard-coded waypoints in `drawDotsToBox`). The screen
  redraws at about 25fps in that state; otherwise it redraws on change and once a second (to keep the DUR box counting).
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
(box x positions and widths, value text centres). The PRE/BRI box and the PRESET/BRIGHT caption
are repainted over the background in code, so the image's own copies of them don't matter.

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
| Brightness jumps back to about 10% on every preset click | The presets have a saved brightness. In WLED edit each preset, untick "Include brightness" and save (or remove the top-level `"bri"` from `presets.json`) |
| No box selected after a reboot | WLED reports `ps` -1 after a brightness change or boot. The firmware shows the last chosen preset (kept in flash) as selected; click once to set it. See `LAST_PRESET_SOLID` |
| Screen flips between offline and online | A dropped request used to flip it. It now takes `OFFLINE_AFTER_FAILS` (3) failed polls in a row; if it still flaps, check WiFi signal at the controller and WLED's load (see the serial log: `WLED offline` / `WLED online`) |
| WLED turns itself off after a while | `AUTO_OFF_MS` (3 hours on, by default). Set it to 0 to disable |
| Build is slow at "Scanning dependencies..." | Libraries and build output must live in the `pio_work` Docker volume, not the Windows folder (`build.cmd` does this). First build after `docker volume rm pio_work` is a full rebuild |
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
