#pragma once

// Tunable settings. Safe to commit. WiFi credentials and the WLED address live in
// secrets.h (not committed).
//
// NOT configurable on purpose: box positions, connector waypoints, the LED ranges of the
// peek ring and TOTAL_LEDS. They are tied to bg.h (the background image) and your window
// layout, so changing them without regenerating the image just breaks the display.

// ---- Display ---------------------------------------------------------------
#define LCD_ROTATION        3     // 3 = USB cable on the left edge, 1 = flipped 180
#define LCD_BRIGHTNESS      200   // 0-255
#define LCD_BRIGHTNESS_OFF  6     // screen backlight while WLED power is off
#define IDLE_DIM_MS         15000 // dim the backlight after this long with no button press (0 = never)
#define LCD_BRIGHTNESS_IDLE 10    // backlight level while idle (0-255)
#define RING_THICKNESS      3     // peek ring thickness in pixels (1-8 sensible)
#define RING_FULL           0
#define RING_BOTTOM_ONLY    1
#define RING_MODE           RING_FULL   // RING_FULL = whole border (LEDs 0-388)
                                        // RING_BOTTOM_ONLY = bottom edge only (LEDs 0-124)

// ---- Features --------------------------------------------------------------
#define PRE_LABEL_SCALE     1.15f // size of the "PRE:" label (1.0 = normal)
#define ENABLE_RGB_LED      1     // onboard RGB LED cycles through the rainbow (off while the screen is dimmed)
#define RGB_LED_PIN         38    // Waveshare ESP32-S3-LCD-1.47: WS2812 on GPIO38 (unconfirmed on your board)
#define RGB_LED_LEVEL       100   // brightness scale 0-255 (the LED is very bright)
#define RGB_LED_CYCLE_MS    6000  // time for one full trip round the rainbow
#define ENABLE_PEEK_RING    1     // 0 = no WebSocket, no ring
#define ENABLE_AUDIO_MARKER 1     // equalizer icon in the corner of boxes for sound-reactive presets
#define LAST_PRESET_SOLID   1     // WLED reports no preset (e.g. after a brightness change): 1 = show the last chosen
                                  // preset as solid selected, 0 = green outline + "(modified)"
#define ENABLE_DOTS         1     // 0 = no connector animation (screen only redraws on change)

// ---- Button ----------------------------------------------------------------
#define LONG_PRESS_MS       2000  // hold this long, then release, to switch Preset <-> Brightness mode
#define POWER_PRESS_MS      4000  // hold this long to toggle WLED power (fires while held)

// ---- Connector animation ---------------------------------------------------
#define DOT_SPACING_PX      14    // gap between dots along the line
#define DOT_MS_PER_PX       25    // lower = faster
#define ANIM_FRAME_MS       40    // redraw interval while animating (40 = 25fps)

// ---- Network timing --------------------------------------------------------
#define STATE_POLL_MS_CFG       2000    // brightness / preset / uptime poll
#define PRESET_REFRESH_MS_CFG   60000   // preset list refresh
#define HOLDOFF_MS_CFG          2500    // ignore polled preset this long after a click
#define PEEK_FRAME_MS           60      // min gap between peek frames (60 = ~15fps)

// ---- Colours (R, G, B) -----------------------------------------------------
#define COL_SELECTED        0, 255, 33
#define COL_UNSELECTED      0, 138, 251
#define COL_DOT             0, 255, 33
#define COL_OFFLINE_TEXT    128, 128, 128
#define COL_CAPTION         253, 230, 2   // yellow of the PRESET/BRIGHT caption
// Brightness mode box colours (preset mode uses COL_SELECTED / COL_UNSELECTED above)
#define COL_SELECTED_BRI    253, 230, 2     // yellow
#define COL_UNSELECTED_BRI  150, 110, 255   // lilac, change to taste
// Lights-off look (connected, but WLED power is off). Dimmed so it differs from offline grey.
#define COL_OFF_SELECTED    0, 80, 14
#define COL_OFF_UNSELECTED  0, 40, 80
#define COL_OFF_TEXT        170, 170, 170
