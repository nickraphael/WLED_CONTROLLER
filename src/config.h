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
#define RING_THICKNESS      3     // peek ring thickness in pixels (1-8 sensible)
#define RING_FULL           0
#define RING_BOTTOM_ONLY    1
#define RING_MODE           RING_FULL   // RING_FULL = whole border (LEDs 0-388)
                                        // RING_BOTTOM_ONLY = bottom edge only (LEDs 0-124)

// ---- Features --------------------------------------------------------------
#define ENABLE_PEEK_RING    1     // 0 = no WebSocket, no ring
#define ENABLE_DOTS         1     // 0 = no connector animation (screen only redraws on change)

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
