#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <map>
#include <esp_system.h>
#include <WebSocketsClient.h>
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
static const uint16_t C_DOT = RGB565(COL_DOT);
static const uint16_t C_GREY = RGB565(COL_OFFLINE_TEXT);

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
  String name;
};

struct Shared {
  bool online = false;
  int bri = 0;  // 0-255
  uint32_t uptime = 0;  // seconds
  int ps = -1;  // current preset id, -1 = none
  int count = 0;
  Preset presets[MAX_PRESETS];
  uint16_t peek[PEEK_MAX] = {0};  // live LED colours (RGB565), one per LED (or every n-th)
  int peekCount = 0;
  uint32_t version = 0;  // bumped on any change that needs a redraw
};

static Shared S;
static SemaphoreHandle_t mtx;
static volatile int pendingPreset = -1;  // preset id the UI wants applied
static volatile uint32_t holdoffUntil = 0;
static volatile int dbgHttp = 0;  // last HTTP result code (debug)

static void lock() { xSemaphoreTake(mtx, portMAX_DELAY); }
static void unlock() { xSemaphoreGive(mtx); }

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
  filter["info"]["uptime"] = true;

  JsonDocument doc;
  if (!httpGetJson("/json/si", doc, filter)) {
    setOffline();
    return false;
  }
  int bri = doc["state"]["bri"] | 0;
  int ps = doc["state"]["ps"] | -1;
  uint32_t uptime = doc["info"]["uptime"] | 0;

  lock();
  bool changed = !S.online || S.bri != bri || S.uptime != uptime;
  bool holding = (int32_t)(holdoffUntil - millis()) > 0;
  if (!holding && S.ps != ps) {
    S.ps = ps;
    changed = true;
  }
  if (!S.online) Serial.println("WLED online");
  S.online = true;
  S.bri = bri;
  S.uptime = uptime;
  if (changed) S.version++;
  unlock();
  return true;
}

static bool fetchPresets() {
  JsonDocument filter;
  filter["*"]["n"] = true;

  JsonDocument doc;
  if (!httpGetJson("/presets.json", doc, filter)) return false;

  std::map<int, String> found;  // sorted by preset id
  for (JsonPair kv : doc.as<JsonObject>()) {
    int id = atoi(kv.key().c_str());
    if (id <= 0) continue;  // "0" is the empty placeholder
    if (!kv.value().is<JsonObject>()) continue;
    const char* n = kv.value()["n"] | "";
    found[id] = (n[0] != '\0') ? String(n) : String("Preset ") + id;
  }

  lock();
  int i = 0;
  for (auto& p : found) {
    if (i >= MAX_PRESETS) break;
    S.presets[i].id = p.first;
    S.presets[i].name = p.second;
    i++;
  }
  S.count = i;
  S.version++;
  unlock();
  Serial.printf("Loaded %d presets (of %d defined)\n", i, (int)found.size());
  return true;
}

static void sendPreset(int id) {
  HTTPClient http;
  http.setConnectTimeout(1500);
  http.setTimeout(2500);
  String url = String("http://") + WLED_HOST + "/json/state";
  if (!http.begin(url)) return;
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(String("{\"ps\":") + id + "}");
  Serial.printf("Apply preset %d -> %d\n", id, code);
  http.end();
}

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

    if (ENABLE_PEEK_RING && !wsStarted) {
      ws.begin(WLED_HOST, 80, "/ws");
      ws.onEvent(wsEvent);
      ws.setReconnectInterval(3000);
      wsStarted = true;
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
  if (!ENABLE_PEEK_RING || !d.online || d.peekCount <= 0) return;
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

static bool hasSelection(const Shared& d) {
  if (!d.online) return false;
  for (int i = 0; i < d.count; i++) if (d.presets[i].id == d.ps) return true;
  return false;
}

static void drawDotsToBox(int box, uint32_t now) {
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
        spr.fillRect(x - 1, y - 1, 3, 3, C_DOT);
        break;
      }
      rem -= len;
    }
  }
}

static void drawUI(const Shared& d, uint32_t now) {
  spr.pushImage(0, 0, BG_W, BG_H, bg565);

  // BRI / DUR value boxes
  spr.setTextDatum(middle_center);
  spr.setFont(&fonts::FreeSansBold9pt7b);
  spr.setTextColor(TFT_WHITE);
  String bri = d.online ? String((d.bri * 100 + 127) / 255) + "%" : String("--");
  String dur = d.online ? fmtUptime(d.uptime) : String("--");
  spr.drawString(bri, 198, 22);
  spr.drawString(dur, 198, 55);

  // Preset number (yellow box, black text)
  String pre = (d.online && d.ps > 0) ? String(d.ps) : String("--");
  spr.setTextColor(TFT_BLACK);
  if (pre.length() <= 1) {
    spr.setFont(&fonts::FreeSansBold12pt7b);
  } else if (pre.length() == 2) {
    spr.setFont(&fonts::FreeSansBold9pt7b);
  } else {
    spr.setFont(&fonts::Font2);
  }
  spr.drawString(pre, 299, 37);

  // Preset boxes. Offline: leave the grey boxes from the background alone.
  String name = d.online ? String("") : String("WLED offline");
  int selBox = -1;
  if (d.online) {
    spr.setFont(&fonts::FreeSansBold12pt7b);
    spr.setTextColor(TFT_WHITE);
    for (int i = 0; i < MAX_PRESETS; i++) {
      int x = BOX_X[i], w = BOX_W[i];
      if (i >= d.count) {
        spr.fillRect(x - 1, BOX_Y - 1, w + 2, BOX_H + 2, TFT_BLACK);  // hide
        continue;
      }
      bool sel = (d.presets[i].id == d.ps);
      spr.fillRoundRect(x, BOX_Y, w, BOX_H, 5, sel ? C_GREEN : C_BLUE);
      spr.drawString(String(i + 1), x + w / 2, BOX_Y + BOX_H / 2);
      if (sel) { name = d.presets[i].name; selBox = i; }
    }
  }
  if (ENABLE_DOTS && selBox >= 0) drawDotsToBox(selBox, now);

  // Selected preset name, bottom strip
  spr.setFont(&fonts::FreeSans9pt7b);
  spr.setTextColor(d.online ? (uint16_t)TFT_WHITE : C_GREY);
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
  if (!S.online || S.count == 0) {
    unlock();
    return;  // offline: button does nothing
  }
  int sel = -1;
  for (int i = 0; i < S.count; i++) {
    if (S.presets[i].id == S.ps) sel = i;
  }
  int next = (sel + 1) % S.count;  // sel == -1 -> first box
  int id = S.presets[next].id;
  S.ps = id;
  S.version++;
  unlock();

  holdoffUntil = millis() + HOLDOFF_MS;
  pendingPreset = id;
}

static void handleButton() {
  static bool prev = true;  // released (pull-up)
  static uint32_t lastChange = 0;
  bool cur = digitalRead(BTN_PIN);
  if (cur != prev && millis() - lastChange > 40) {
    lastChange = millis();
    prev = cur;
    if (!cur) onClick();  // press
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BTN_PIN, INPUT_PULLUP);
  mtx = xSemaphoreCreateMutex();

  lcd.init();
  lcd.setRotation(LCD_ROTATION);  // landscape 320x172, flipped 180 (use 1 for the other way)
  lcd.setBrightness(LCD_BRIGHTNESS);
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
  uint32_t now = millis();
  if (changed) drawnVersion = snap.version;
  if (changed || (ENABLE_DOTS && hasSelection(snap) && now - lastFrame >= ANIM_FRAME_MS)) {
    lastFrame = now;
    drawUI(snap, now);
  }
  delay(5);
}
