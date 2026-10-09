#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <map>
#include <esp_system.h>
#include <WebSocketsClient.h>
#include <Preferences.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "bg.h"
#include "secrets.h"   // WIFI_SSID, WIFI_PASS, WLED_HOST
#include "config.h"

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
static const int BTN_PIN = 0;  // BOOT button, active low
static const int MAX_PRESETS = 8;
static const int PEEK_MAX = 1024;  // WLED live frame: up to 1024 samples on ESP32 (256 on ESP8266)
static const uint32_t STATE_POLL_MS = STATE_POLL_MS_CFG;
static const uint32_t PRESET_REFRESH_MS = PRESET_REFRESH_MS_CFG;
static const uint32_t HOLDOFF_MS = HOLDOFF_MS_CFG;  // ignore polled preset after a click

// Preset boxes in the 320x172 background (measured from the mockup)
static const int BOX_X[MAX_PRESETS] = {10, 50, 88, 126, 162, 199, 236, 274};
static const int BOX_W[MAX_PRESETS] = {37, 35, 35, 34, 34, 34, 35, 36};
static const int BOX_Y = 101;
static const int BOX_H = 41;

constexpr uint16_t RGB565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static const uint16_t C_BLUE = RGB565(COL_UNSELECTED);
static const uint16_t C_GREEN = RGB565(COL_SELECTED);
static const uint16_t C_BRI_SEL = RGB565(COL_SELECTED_BRI);
static const uint16_t C_BRI_UNSEL = RGB565(COL_UNSELECTED_BRI);
static const uint16_t C_DOT = RGB565(COL_DOT);
static const uint16_t C_GREY = RGB565(COL_OFFLINE_TEXT);
static const uint16_t C_OFF_SEL = RGB565(COL_OFF_SELECTED);
static const uint16_t C_OFF_UNSEL = RGB565(COL_OFF_UNSELECTED);
static const uint16_t C_OFF_TEXT = RGB565(COL_OFF_TEXT);

// ---------------------------------------------------------------------------
// Display (Waveshare ESP32-S3-LCD-1.47, ST7789 172x320) - confirmed working
// ---------------------------------------------------------------------------
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;

public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.pin_sclk = 40;
      cfg.pin_mosi = 45;
      cfg.pin_miso = -1;
      cfg.pin_dc = 41;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = 42;
      cfg.pin_rst = 39;
      cfg.pin_busy = -1;
      cfg.panel_width = 172;
      cfg.panel_height = 320;
      cfg.offset_x = 34;
      cfg.offset_y = 0;
      cfg.invert = true;
      cfg.rgb_order = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = 46;
      cfg.invert = false;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};

LGFX lcd;
LGFX_Sprite spr(&lcd);

// ---------------------------------------------------------------------------
// Shared state (network task on core 0 writes, UI loop on core 1 reads)
// ---------------------------------------------------------------------------
struct Preset {
  int id = 0;
  bool audio = false;  // uses a sound-reactive effect (see fetchFxData)
  String name;
};

struct Shared {
  bool online = false;
  int bri = 0;  // 0-255
  uint32_t uptime = 0;  // seconds
  int ps = -1;  // current preset id, -1 = none
  int mode = 0;  // UiMode: what the button and boxes control
  bool on = true;  // WLED power state (state.on)
  int lastPs = -1;  // last preset the user chose (kept in flash), shown outlined when WLED reports none
  int hold = 0;  // 1 = button held past LONG_PRESS_MS (show the hint), 0 = otherwise
  int count = 0;
  Preset presets[MAX_PRESETS];
  uint16_t peek[PEEK_MAX] = {0};  // live LED colours (RGB565), one per LED (or every n-th)
  int peekCount = 0;
  uint32_t version = 0;  // bumped on any change that needs a redraw
};

enum UiMode { MODE_PRESET = 0, MODE_BRIGHTNESS = 1 };

static Shared S;
static SemaphoreHandle_t mtx;
static volatile int pendingPreset = -1;  // preset id the UI wants applied
static volatile int pendingBri = -1;     // brightness (0-255) the UI wants applied
static volatile int pendingPower = -1;   // 1 = on, 0 = off, -1 = nothing
static volatile bool peekWanted = true;  // false while the screen is dimmed: websocket closed, ring hidden
static volatile uint32_t holdoffUntil = 0;
static volatile int dbgHttp = 0;  // last HTTP result code (debug)

static void lock() { xSemaphoreTake(mtx, portMAX_DELAY); }
static void unlock() { xSemaphoreGive(mtx); }

static volatile uint32_t lastActivity = 0;  // millis() of the last button activity (for the idle dim)

// Last chosen preset id, remembered across reboots (only written when it changes).
static void savePs(int id) {
  Preferences pr;
  if (pr.begin("wledctl", false)) {
    pr.putInt("lastps", id);
    pr.end();
  }
}

// ---- Brightness levels: box k (0-7) = (k+1)/8 of full, i.e. 12.5% .. 100% ----
static const int BRI_LEVELS = MAX_PRESETS;  // one box per level
static int briForLevel(int k) { return (255 * (k + 1) + BRI_LEVELS / 2) / BRI_LEVELS; }
static String briPercentText(int k) {
  float pct = 100.0f * (k + 1) / BRI_LEVELS;
  return String(pct, (k + 1) * 100 % BRI_LEVELS == 0 ? 0 : 1);
}
// Text for a raw brightness: the exact level text (12.5, 25, 37.5 ...) when it sits on a level,
// otherwise the rounded percentage (e.g. bri 26 -> "10").
static String briDisplayText(int bri) {
  for (int k = 0; k < BRI_LEVELS; k++)
    if (briForLevel(k) == bri) return briPercentText(k);
  return String((bri * 100 + 127) / 255);
}
// Whole-number percent for the small boxes. Exact levels round half up (12.5 -> 13, 87.5 -> 88),
// so the header agrees with the level the user stepped to.
static String briRoundedText(int bri) {
  for (int k = 0; k < BRI_LEVELS; k++)
    if (briForLevel(k) == bri) return String((100 * (k + 1) + BRI_LEVELS / 2) / BRI_LEVELS);
  return String((bri * 100 + 127) / 255);
}
// Nearest level to a raw brightness, -1 when WLED is at 0 (so a click goes to the first level).
static int levelForBri(int bri) {
  if (bri <= 0) return -1;
  int best = 0, bestDiff = 1000;
  for (int k = 0; k < BRI_LEVELS; k++) {
    int diff = abs(briForLevel(k) - bri);
    if (diff < bestDiff) { bestDiff = diff; best = k; }
  }
  return best;
}

// ---------------------------------------------------------------------------
// WLED HTTP
// ---------------------------------------------------------------------------
static bool httpGetJson(const char* path, JsonDocument& doc, const JsonDocument& filter) {
  HTTPClient http;
  http.setConnectTimeout(1500);
  http.setTimeout(2500);
  http.useHTTP10(true);  // avoids chunked encoding, which ArduinoJson can't read
  String url = String("http://") + WLED_HOST + path;
  if (!http.begin(url)) { dbgHttp = -100; return false; }
  int code = http.GET();
  dbgHttp = code;
  bool ok = false;
  if (code == 200) {
    DeserializationError err =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    ok = !err;
    if (err) Serial.printf("JSON error on %s: %s\n", path, err.c_str());
  } else {
    Serial.printf("GET %s -> %d\n", path, code);
  }
  http.end();
  return ok;
}

static void setOffline() {
  lock();
  if (S.online) {
    S.online = false;
    S.peekCount = 0;
    S.version++;
    Serial.println("WLED offline");
  }
  unlock();
}

static bool pollState() {
  JsonDocument filter;
  filter["state"]["bri"] = true;
  filter["state"]["ps"] = true;
  filter["state"]["on"] = true;
  filter["info"]["uptime"] = true;

  JsonDocument doc;
  if (!httpGetJson("/json/si", doc, filter)) {
    setOffline();
    return false;
  }
  int bri = doc["state"]["bri"] | 0;
  int ps = doc["state"]["ps"] | -1;
  bool on = doc["state"]["on"] | true;
  uint32_t uptime = doc["info"]["uptime"] | 0;

  int saveId = -1;
  lock();
  bool changed = !S.online || S.uptime != uptime;
  bool holding = (int32_t)(holdoffUntil - millis()) > 0;
  if (!holding && S.ps != ps) {
    S.ps = ps;
    changed = true;
    if (ps > 0 && S.lastPs != ps) {
      S.lastPs = ps;
      saveId = ps;
    }
  }
  if (!holding && S.on != on) {
    S.on = on;
    if (!on) S.mode = MODE_PRESET;
    changed = true;
  }
  if (!holding && S.bri != bri) {
    S.bri = bri;
    changed = true;
  }
  if (!S.online) Serial.println("WLED online");
  S.online = true;
  S.uptime = uptime;
  if (changed) S.version++;
  unlock();
  if (saveId > 0) savePs(saveId);
  return true;
}

// ---- Which effects react to the microphone -------------------------------------------------
// GET /json/fxdata is an array of strings, one per effect id: "sliders;colors;palette;flags;defaults".
// The flags field contains 'v' (volume reactive) and/or 'f' (frequency reactive) for audio effects.
static const int FX_MAX = 256;
static bool audioFx[FX_MAX];

static bool fxIsAudio(const char* meta) {
  int semis = 0;
  const char* c = meta;
  while (*c && semis < 3) {
    if (*c == ';') semis++;
    c++;
  }
  if (semis < 3) return false;
  for (; *c && *c != ';'; c++) {
    if (*c == 'v' || *c == 'f') return true;
  }
  return false;
}

static bool fetchFxData() {
  JsonDocument filter;
  filter.add(true);  // keep every array element (the strings)
  JsonDocument doc;
  if (!httpGetJson("/json/fxdata", doc, filter)) return false;
  JsonArray arr = doc.as<JsonArray>();
  if (arr.isNull() || arr.size() == 0) return false;
  memset(audioFx, 0, sizeof(audioFx));
  int i = 0, n = 0;
  for (JsonVariant v : arr) {
    if (i < FX_MAX && fxIsAudio(v.as<const char*>() ? v.as<const char*>() : "")) {
      audioFx[i] = true;
      n++;
    }
    i++;
  }
  Serial.printf("fxdata: %d effects, %d audio-reactive\n", i, n);
  return true;
}

static bool fetchPresets() {
  JsonDocument filter;
  filter["*"]["n"] = true;
  filter["*"]["seg"][0]["fx"] = true;  // applies to every segment

  JsonDocument doc;
  if (!httpGetJson("/presets.json", doc, filter)) return false;

  std::map<int, String> found;  // sorted by preset id
  std::map<int, bool> foundAudio;
  for (JsonPair kv : doc.as<JsonObject>()) {
    int id = atoi(kv.key().c_str());
    if (id <= 0) continue;  // "0" is the empty placeholder
    if (!kv.value().is<JsonObject>()) continue;
    const char* n = kv.value()["n"] | "";
    found[id] = (n[0] != '\0') ? String(n) : String("Preset ") + id;
    bool au = false;  // audio if any segment's effect is sound-reactive
    for (JsonObject sg : kv.value()["seg"].as<JsonArray>()) {
      int fx = sg["fx"] | -1;
      if (fx >= 0 && fx < FX_MAX && audioFx[fx]) au = true;
    }
    foundAudio[id] = au;
  }

  lock();
  int i = 0;
  for (auto& p : found) {
    if (i >= MAX_PRESETS) break;
    S.presets[i].id = p.first;
    S.presets[i].name = p.second;
    S.presets[i].audio = foundAudio[p.first];
    i++;
  }
  S.count = i;
  S.version++;
  unlock();
  Serial.printf("Loaded %d presets (of %d defined)\n", i, (int)found.size());
  return true;
}

static void sendState(const String& json) {
  HTTPClient http;
  http.setConnectTimeout(1500);
  http.setTimeout(2500);
  String url = String("http://") + WLED_HOST + "/json/state";
  if (!http.begin(url)) return;
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(json);
  Serial.printf("POST %s -> %d\n", json.c_str(), code);
  http.end();
}
static void sendPreset(int id) { sendState(String("{\"ps\":") + id + "}"); }
static void sendPower(bool on) { sendState(on ? String("{\"on\":true}") : String("{\"on\":false}")); }
static void sendBri(int bri) { sendState(String("{\"bri\":") + bri + "}"); }

// WLED live view ("peek") over WebSocket: send {"lv":true}, receive binary frames
// 'L', version(1), then RGB triplets for the strip (WLED subsamples to <=256 LEDs).
static WebSocketsClient ws;
static uint32_t lastPeekMs = 0;

static void wsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      ws.sendTXT("{\"lv\":true}");
      Serial.println("WS connected");
      break;
    case WStype_DISCONNECTED:
      lock();
      S.peekCount = 0;
      S.version++;
      unlock();
      break;
    case WStype_BIN: {
      if (length < 5 || payload[0] != 'L' || payload[1] != 1) break;  // 1D frames only
      uint32_t now = millis();
      if (now - lastPeekMs < PEEK_FRAME_MS) break;  // ~15 fps is plenty
      lastPeekMs = now;
      int n = (int)((length - 2) / 3);
      if (n > PEEK_MAX) n = PEEK_MAX;
      lock();
      for (int i = 0; i < n; i++) {
        const uint8_t* p = payload + 2 + i * 3;
        S.peek[i] = RGB565(p[0], p[1], p[2]);
      }
      S.peekCount = n;
      S.version++;
      unlock();
      break;
    }
    default:
      break;
  }
}

static void netTask(void*) {
  uint32_t lastPresetOk = 0, lastPresetTry = 0, lastState = 0, lastWifiTry = 0;
  bool havePresets = false;
  bool haveFx = false;
  uint32_t lastFxTry = 0;
  bool wsStarted = false;

  for (;;) {
    uint32_t now = millis();

    if (WiFi.status() != WL_CONNECTED) {
      setOffline();
      if (now - lastWifiTry > 15000) {
        lastWifiTry = now;
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASS);
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    int id = pendingPreset;
    if (id >= 0) {
      pendingPreset = -1;
      sendPreset(id);
      lastState = 0;  // re-poll straight away
    }
    int pw = pendingPower;
    if (pw >= 0) {
      pendingPower = -1;
      sendPower(pw == 1);
      lastState = 0;
    }
    int b = pendingBri;
    if (b >= 0) {
      pendingBri = -1;
      sendBri(b);
      lastState = 0;
    }

    if (!haveFx && (lastFxTry == 0 || now - lastFxTry > 5000)) {
      lastFxTry = now;
      if (fetchFxData()) {
        haveFx = true;
        havePresets = false;  // re-read presets so the audio flags are filled in
      }
    }

    bool needPresets = !havePresets || (now - lastPresetOk > PRESET_REFRESH_MS);
    if (needPresets && (now - lastPresetTry > 5000 || lastPresetTry == 0)) {
      lastPresetTry = now;
      if (fetchPresets()) {
        havePresets = true;
        lastPresetOk = millis();
      }
    }

    if (now - lastState >= STATE_POLL_MS || lastState == 0) {
      lastState = now;
      pollState();
    }

    bool wantPeek = ENABLE_PEEK_RING && peekWanted;
    if (wantPeek && !wsStarted) {
      ws.begin(WLED_HOST, 80, "/ws");
      ws.onEvent(wsEvent);
      ws.setReconnectInterval(3000);
      wsStarted = true;
    }
    if (!wantPeek && wsStarted) {  // screen dimmed: close the websocket and drop the ring
      ws.disconnect();
      wsStarted = false;  // ws.loop() is no longer called, so it won't auto-reconnect
      lock();
      S.peekCount = 0;
      S.version++;
      unlock();
    }
    if (wsStarted) ws.loop();

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------
static String fmtUptime(uint32_t s) {
  if (s < 60) return String(s) + "s";
  if (s < 3600) return String(s / 60) + "m";
  if (s < 86400) return String(s / 3600) + "h";
  return String(s / 86400) + "d";
}

// ---- Live LED "peek" drawn as a 3px ring around the screen edge ------------
// LED layout (anticlockwise from bottom-left): bottom 0-124, right 125-194,
// top 195-319, left 320-388. WLED's live frame is one flat list in LED order
// (no segment info), subsampled to every n-th LED when there are >256 LEDs.
static const int TOTAL_LEDS = 389;
static const int RING = RING_THICKNESS;

static inline uint16_t peekColor(const Shared& d, int led, int n) {
  int k = led / n;
  if (k >= d.peekCount) k = d.peekCount - 1;
  return d.peek[k];
}

static void drawPeekRing(const Shared& d) {
  if (!ENABLE_PEEK_RING || !d.online || !d.on || d.peekCount <= 0) return;
  // WLED sends floor(TOTAL_LEDS / n) samples, so find the n that gives our sample count.
  int n = (TOTAL_LEDS + d.peekCount - 1) / d.peekCount;  // fallback if nothing matches
  for (int t = 1; t <= 8; t++) {
    if (TOTAL_LEDS / t == d.peekCount) {
      n = t;
      break;
    }
  }
  const int W = BG_W, H = BG_H;
  const int sideH = H - 2 * RING;  // sides run between the top and bottom rings

  for (int x = 0; x < W; x++) {  // bottom: left -> right, LEDs 0-124
    spr.fillRect(x, H - RING, 1, RING, peekColor(d, x * 125 / W, n));
  }
  if (RING_MODE == RING_BOTTOM_ONLY) return;  // bottom edge only, skip the other three
  for (int x = 0; x < W; x++) {  // top: right -> left, LEDs 195-319
    spr.fillRect(x, 0, 1, RING, peekColor(d, 195 + (W - 1 - x) * 125 / W, n));
  }
  for (int j = 0; j < sideH; j++) {  // right: bottom -> top, LEDs 125-194
    spr.fillRect(W - RING, H - RING - 1 - j, RING, 1, peekColor(d, 125 + j * 70 / sideH, n));
  }
  for (int j = 0; j < sideH; j++) {  // left: top -> bottom, LEDs 320-388
    spr.fillRect(0, RING + j, RING, 1, peekColor(d, 320 + j * 69 / sideH, n));
  }
}

// ---- Green dots flowing from the PRE box down the connector to the selected box
struct Pt { int x, y; };
static const int DROP_X[MAX_PRESETS] = {28, 66, 105, 142, 178, 215, 253, 288};
static const int SHARED_X = 278, SHARED_Y = 80;
static const int DOT_SPACING = DOT_SPACING_PX;

// The preset id the UI treats as selected: WLED's own `ps`, or (when WLED reports none, e.g. after a
// brightness change) the remembered last-chosen one if it still exists. -1 = nothing selected.
static int effectivePs(const Shared& d) {
  if (d.ps > 0) return d.ps;
  if (LAST_PRESET_SOLID)
    for (int i = 0; i < d.count; i++) if (d.presets[i].id == d.lastPs) return d.lastPs;
  return -1;
}

static bool hasSelection(const Shared& d) {
  if (!d.online || !d.on) return false;  // no animation while the lights are off
  if (d.mode == MODE_BRIGHTNESS) return true;
  for (int i = 0; i < d.count; i++) if (d.presets[i].id == d.ps) return true;
  if (LAST_PRESET_SOLID && d.ps <= 0)
    for (int i = 0; i < d.count; i++) if (d.presets[i].id == d.lastPs) return true;
  return false;
}

static void drawDotsToBox(int box, uint32_t now, uint16_t col) {
  Pt p[4]; int np;
  if (box == 0)      { p[0] = {268, 54}; p[1] = {268, 75}; p[2] = {DROP_X[0], 75}; p[3] = {DROP_X[0], 95}; np = 4; }
  else if (box == 7) { p[0] = {288, 54}; p[1] = {288, 95}; np = 2; }
  else               { p[0] = {SHARED_X, 54}; p[1] = {SHARED_X, SHARED_Y}; p[2] = {DROP_X[box], SHARED_Y}; p[3] = {DROP_X[box], 95}; np = 4; }
  int total = 0;
  for (int i = 0; i + 1 < np; i++) total += abs(p[i + 1].x - p[i].x) + abs(p[i + 1].y - p[i].y);
  int off = (now / (uint32_t)DOT_MS_PER_PX) % DOT_SPACING;
  for (int d = off; d < total; d += DOT_SPACING) {
    int rem = d;
    for (int i = 0; i + 1 < np; i++) {
      int dx = p[i + 1].x - p[i].x, dy = p[i + 1].y - p[i].y;
      int len = abs(dx) + abs(dy);
      if (rem < len) {
        int x = p[i].x + (dx > 0 ? rem : dx < 0 ? -rem : 0);
        int y = p[i].y + (dy > 0 ? rem : dy < 0 ? -rem : 0);
        spr.fillRect(x - 1, y - 1, 3, 3, col);
        break;
      }
      rem -= len;
    }
  }
}

// Tiny 3-bar equalizer (about 8x8) marking a sound-reactive preset, drawn in a box corner.
static void drawAudioIcon(int x, int y, uint16_t col) {
  spr.fillRect(x, y + 4, 2, 4, col);
  spr.fillRect(x + 3, y, 2, 8, col);
  spr.fillRect(x + 6, y + 2, 2, 6, col);
}

static void drawUI(const Shared& d, uint32_t now) {
  spr.pushImage(0, 0, BG_W, BG_H, bg565);

  // BRI / DUR value boxes
  spr.setTextDatum(middle_center);
  spr.setFont(&fonts::FreeSansBold9pt7b);
  spr.setTextColor(TFT_WHITE);
  String bri = d.online ? briRoundedText(d.bri) + "%" : String("--");
  String dur = d.online ? fmtUptime(d.uptime) : String("--");
  spr.drawString(bri, 198, 22);
  spr.drawString(dur, 198, 55);

  bool off = d.online && !d.on;  // connected, but WLED power is off
  bool briMode = d.online && d.on && d.mode == MODE_BRIGHTNESS;

  // Caption + yellow box are repainted here in both modes, so the "PRESET"/"PRE:" text
  // baked into bg.h (if still present) is covered, and a blank background works too.
  spr.fillRect(246, 5, 52, 15, RGB565(3, 7, 8));
  spr.setFont(&fonts::Font2);  // small caption (the old baked-in one was ~11px tall)
  spr.setTextDatum(middle_left);
  spr.setTextColor(RGB565(COL_CAPTION));
  spr.drawString(briMode ? "BRIGHT" : "PRESET", 249, 12);

  // The whole box (x 242-313, y 23-49 in the background) is repainted, slightly oversized so
  // the baked-in yellow edge is covered. Colour matches the selected-box colour of the mode.
  spr.fillRoundRect(241, 22, 74, 29, 6, !d.online ? C_GREY : off ? C_OFF_SEL : (briMode ? C_BRI_SEL : C_GREEN));  // grey offline, dim when off
  spr.setTextColor(off ? C_OFF_TEXT : (uint16_t)TFT_BLACK);
  const char* label = briMode ? "BR:" : "PRE:";
  spr.setFont(&fonts::FreeSansBold9pt7b);
  spr.setTextSize(briMode ? 1.0f : PRE_LABEL_SCALE);
  const int labelX = briMode ? 245 : 247;
  spr.drawString(label, labelX, 37);
  int labelEnd = labelX + spr.textWidth(label);
  spr.setTextSize(1.0f);

  String val;
  if (briMode) {
    val = briRoundedText(d.bri);  // real brightness (whole percent), not the snapped level
  } else {
    int eps = effectivePs(d);
    val = off ? String("OFF") : (d.online && eps > 0) ? String(eps) : String("--");
  }
  // Biggest font that fits in the space right of the label.
  const int valLeft = labelEnd + (briMode ? 1 : 3), valRight = briMode ? 311 : 309;  // brightness text is longer (e.g. 87.5)
  int avail = valRight - valLeft;
  // Brightness mode appends a small "%" (Font2) after the digits, so reserve room for it.
  int pctW = 0;
  if (briMode) {
    spr.setFont(&fonts::Font2);
    pctW = spr.textWidth("%") + 1;
  }
  const lgfx::IFont* vf = &fonts::Font2;
  const lgfx::IFont* cand[] = {&fonts::FreeSansBold12pt7b, &fonts::FreeSansBold9pt7b};
  for (auto f : cand) {
    spr.setFont(f);
    if (spr.textWidth(val) + pctW <= avail) { vf = f; break; }
  }
  spr.setFont(vf);
  if (briMode) {
    int dw = spr.textWidth(val);
    int sx = (valLeft + valRight) / 2 - (dw + pctW) / 2;
    spr.setTextDatum(middle_left);
    spr.drawString(val, sx, 37);
    spr.setFont(&fonts::Font2);
    spr.drawString("%", sx + dw + 1, 37);
  } else {
    spr.setTextDatum(middle_center);
    spr.drawString(val, (valLeft + valRight) / 2, 37);  // centred in the space right of the label
  }
  if (off) {  // lights off: the box just says OFF, no PRE: label
    spr.fillRoundRect(241, 22, 74, 29, 6, C_OFF_SEL);
    spr.setFont(&fonts::FreeSansBold12pt7b);
    spr.setTextColor(C_OFF_TEXT);
    spr.setTextDatum(middle_center);
    spr.drawString("OFF", 278, 37);
  }
  spr.setTextDatum(middle_center);

  // Preset boxes. Offline: leave the grey boxes from the background alone.
  String name = d.online ? String("") : String("WLED offline");
  int selBox = -1;
  if (d.online) {
    spr.setFont(&fonts::FreeSansBold12pt7b);
    spr.setTextColor(TFT_WHITE);
    int shown = briMode ? BRI_LEVELS : d.count;
    int curLevel = levelForBri(d.bri);
    for (int i = 0; i < MAX_PRESETS; i++) {
      int x = BOX_X[i], w = BOX_W[i];
      if (i >= shown) {
        spr.fillRect(x - 1, BOX_Y - 1, w + 2, BOX_H + 2, TFT_BLACK);  // hide
        continue;
      }
      bool sel = briMode ? (i == curLevel) : (d.presets[i].id == d.ps);
      // WLED reports no preset (state was modified): outline the one the user last chose.
      bool ghost = !briMode && !off && !sel && d.ps <= 0 && d.presets[i].id == d.lastPs;
      bool ghostSolid = ghost && LAST_PRESET_SOLID;  // remembered preset treated as selected
      bool lit = sel || ghostSolid;
      uint16_t selCol = off ? C_OFF_SEL : briMode ? C_BRI_SEL : C_GREEN;
      uint16_t unselCol = off ? C_OFF_UNSEL : briMode ? C_BRI_UNSEL : C_BLUE;
      spr.fillRoundRect(x, BOX_Y, w, BOX_H, 5, lit ? selCol : unselCol);
      if (ghost && !LAST_PRESET_SOLID) {
        spr.drawRoundRect(x, BOX_Y, w, BOX_H, 5, C_GREEN);
        spr.drawRoundRect(x + 1, BOX_Y + 1, w - 2, BOX_H - 2, 4, C_GREEN);
      }
      spr.setTextColor(off ? C_OFF_TEXT : lit ? (uint16_t)TFT_BLACK : (uint16_t)TFT_WHITE);  // black on the selected box
      spr.drawString(String(i + 1), x + w / 2, BOX_Y + BOX_H / 2);
      if (ENABLE_AUDIO_MARKER && !briMode && !off && d.presets[i].audio)  // every sound-reactive preset, selected or not
        drawAudioIcon(x + w - 11, BOX_Y + 4, lit ? (uint16_t)TFT_BLACK : (uint16_t)TFT_WHITE);
      if (ghost && !LAST_PRESET_SOLID) name = d.presets[i].name + " (modified)";
      if (sel || ghostSolid) { name = briMode ? String("Brightness ") + briDisplayText(d.bri) + "%" : d.presets[i].name; selBox = i; }
    }
  }
  if (ENABLE_DOTS && selBox >= 0 && !off) drawDotsToBox(selBox, now, briMode ? C_BRI_SEL : C_DOT);

  // Bottom strip: preset name, or a hint / status line
  if (off) name = "Lights off - click to turn on";
  if (d.online && d.hold == 1) name = off ? "Keep holding to turn on" : "Release: mode  |  keep holding: power off";
  spr.setFont(&fonts::FreeSans9pt7b);
  spr.setTextColor((d.online && !off) ? (uint16_t)TFT_WHITE : C_GREY);
  if (spr.textWidth(name) > 300) {
    while (spr.textWidth(name + "..") > 300 && name.length() > 1) name.remove(name.length() - 1);
    name += "..";
  }
  spr.drawString(name, 160, 155);

  drawPeekRing(d);

  spr.pushSprite(0, 0);
}

static void onClick() {
  lock();
  if (!S.online) {
    unlock();
    return;  // offline: button does nothing
  }
  if (!S.on) {  // a plain click turns the lights back on
    S.on = true;
    S.version++;
    unlock();
    holdoffUntil = millis() + HOLDOFF_MS;
    pendingPower = 1;
    return;
  }
  if (S.mode == MODE_BRIGHTNESS) {
    int next = (levelForBri(S.bri) + 1) % BRI_LEVELS;  // -1 (off) -> first level
    int bri = briForLevel(next);
    S.bri = bri;
    S.version++;
    unlock();
    holdoffUntil = millis() + HOLDOFF_MS;
    pendingBri = bri;
    return;
  }
  if (S.count == 0) {
    unlock();
    return;
  }
  int sel = -1;
  for (int i = 0; i < S.count; i++) {
    if (S.presets[i].id == S.ps) sel = i;
  }
  if (sel < 0) {  // WLED reports no preset: continue from the one last chosen
    for (int i = 0; i < S.count; i++) {
      if (S.presets[i].id == S.lastPs) sel = i;
    }
  }
  int next = (sel + 1) % S.count;  // sel == -1 -> first box
  int id = S.presets[next].id;
  S.ps = id;
  S.lastPs = id;
  S.version++;
  unlock();
  savePs(id);

  holdoffUntil = millis() + HOLDOFF_MS;
  pendingPreset = id;
}

static void onLongPress() {
  lock();
  if (!S.online) {
    unlock();
    return;  // offline: button does nothing
  }
  if (!S.on) {
    unlock();
    return;  // lights off: the mode switch is meaningless
  }
  S.mode = (S.mode == MODE_PRESET) ? MODE_BRIGHTNESS : MODE_PRESET;
  S.version++;
  unlock();
}

static void togglePower() {
  lock();
  if (!S.online) {
    unlock();
    return;
  }
  S.on = !S.on;
  if (!S.on) S.mode = MODE_PRESET;
  bool on = S.on;
  S.version++;
  unlock();
  holdoffUntil = millis() + HOLDOFF_MS;
  pendingPower = on ? 1 : 0;
}

static void setHold(int stage) {
  lock();
  if (S.hold != stage) {
    S.hold = stage;
    S.version++;
  }
  unlock();
}

// Button gestures (short press acts on RELEASE so a long press can't also fire a click):
//   short press        click: next preset / brightness level, or turn the lights on if off
//   hold 2s, release   switch Preset <-> Brightness mode (hint shown from 2s)
//   hold 4s            toggle WLED power (fires while still held)
static void handleButton() {
  static bool prev = true;  // released (pull-up)
  static uint32_t lastChange = 0, pressedAt = 0;
  static int stage = 0;  // 0 = short so far, 1 = past LONG_PRESS_MS, 2 = power fired
  bool cur = digitalRead(BTN_PIN);
  uint32_t now = millis();
  if (cur != prev && now - lastChange > 40) {
    lastChange = now;
    prev = cur;
    lastActivity = now;
    if (!cur) {  // pressed
      pressedAt = now;
      stage = 0;
    } else {  // released
      int st = stage;
      stage = 0;
      setHold(0);
      if (st == 0) onClick();
      else if (st == 1) onLongPress();
    }
  }
  if (!prev) {
    lastActivity = now;  // still holding: not idle
    uint32_t held = now - pressedAt;
    if (stage == 0 && held >= LONG_PRESS_MS) {
      stage = 1;
      setHold(1);
    }
    if (stage == 1 && held >= POWER_PRESS_MS) {
      stage = 2;
      setHold(0);
      togglePower();
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BTN_PIN, INPUT_PULLUP);
  mtx = xSemaphoreCreateMutex();
  {
    Preferences pr;
    if (pr.begin("wledctl", false)) {
      S.lastPs = pr.getInt("lastps", -1);
      pr.end();
    }
  }

  lcd.init();
  lcd.setRotation(LCD_ROTATION);  // landscape 320x172, flipped 180 (use 1 for the other way)
  lcd.setBrightness(LCD_BRIGHTNESS);
  lastActivity = millis();  // the idle dim counts from boot
  lcd.fillScreen(TFT_BLACK);

  spr.setColorDepth(16);
  if (!spr.createSprite(BG_W, BG_H)) Serial.println("Sprite alloc failed");

  Shared first;
  drawUI(first, millis());  // offline/grey screen while WiFi connects

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);
}

// ---- Onboard RGB LED (single WS2812, GPIO RGB_LED_PIN): slow rainbow cycle -------------------
// hue 0..1535 around the six colour sectors; level scales the brightness (the LED is very bright).
static void rainbow(uint16_t hue, uint8_t level, uint8_t& r, uint8_t& g, uint8_t& b) {
  uint8_t sector = hue / 256, f = hue % 256;
  uint8_t R = 0, G = 0, B = 0;
  switch (sector) {
    case 0: R = 255; G = f; break;
    case 1: R = 255 - f; G = 255; break;
    case 2: G = 255; B = f; break;
    case 3: G = 255 - f; B = 255; break;
    case 4: R = f; B = 255; break;
    default: R = 255; B = 255 - f; break;
  }
  r = (uint16_t)R * level / 255;
  g = (uint16_t)G * level / 255;
  b = (uint16_t)B * level / 255;
}

// Cycles while active, switched off while the screen is dimmed. Writes only when the value changes.
static void updateRgbLed(bool active, uint32_t now) {
  if (!ENABLE_RGB_LED) return;
  static uint32_t lastWrite = 0;
  static bool wasOn = true;  // forces a first write
  static bool first = true;
  if (!active) {
    if (wasOn || first) {
      neopixelWrite(RGB_LED_PIN, 0, 0, 0);
      wasOn = false;
      first = false;
    }
    return;
  }
  if (now - lastWrite < 30) return;  // ~33 updates/s is plenty
  lastWrite = now;
  uint8_t r, g, b;
  rainbow((now % RGB_LED_CYCLE_MS) * 1536UL / RGB_LED_CYCLE_MS, RGB_LED_LEVEL, r, g, b);
  neopixelWrite(RGB_LED_PIN, r, g, b);
  wasOn = true;
  first = false;
}

void loop() {
  handleButton();

  static uint32_t drawnVersion = 0xFFFFFFFF;
  static Shared snap;  // only copied when something changed (avoids String churn)
  bool changed = false;
  lock();
  if (S.version != drawnVersion) {
    snap = S;
    changed = true;
  }
  unlock();
  static uint32_t lastFrame = 0;
  static int lastBacklight = -1;
  uint32_t now = millis();
  if (changed) drawnVersion = snap.version;
  {
    // Backlight only: dim after IDLE_DIM_MS without a button press; everything else keeps running.
    bool off = snap.online && !snap.on;
    bool idle = IDLE_DIM_MS > 0 && (now - lastActivity) >= IDLE_DIM_MS;
    int bl = off ? LCD_BRIGHTNESS_OFF : idle ? LCD_BRIGHTNESS_IDLE : LCD_BRIGHTNESS;
    if (bl != lastBacklight) {
      lcd.setBrightness(bl);
      lastBacklight = bl;
    }
    updateRgbLed(!off && !idle, now);  // rainbow only while the screen is fully bright
    peekWanted = !off && !idle;        // same for the live peek (websocket + ring)
  }
  if (changed || (ENABLE_DOTS && hasSelection(snap) && now - lastFrame >= ANIM_FRAME_MS)) {
    lastFrame = now;
    drawUI(snap, now);
  }
  delay(5);
}
