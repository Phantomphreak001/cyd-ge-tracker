// GE Tracker - CYD (ESP32-2432S028R) mini OSRS Grand Exchange ticker
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <time.h>
#include "items.h"

#define UA "cyd-ge-ticker - Phantomphreak001 GitHub"
#define API "https://prices.runescape.wiki/api/v1/osrs/"
#define REFRESH_MS 60000UL
#define VOL_FLOOR 20        // min 1h trade volume (buy+sell) for flips
#define ROI_FLOOR 1.0f      // min ROI % for new-player flips
#define MAX_BUY 2000000     // new-player affordability cap (gp)
#define HIGH_ROLLERS 12     // items in ticker (top raw margin)
#define TICKER_STEP_PX 1    // ticker pixels per frame
#define TICKER_FRAME_MS 25  // frame interval -> 40 px/s (was 80 px/s)
// touch (XPT2046 on its own SPI bus)
#define T_CLK 25
#define T_MISO 39
#define T_MOSI 32
#define T_CS 33
#define T_IRQ 36

// colours (dark theme)
#define C_BG    0x0841
#define C_PANEL 0x18E3
#define C_GOLD  0xFE60
#define C_TXT   0xDEFB
#define C_DIM   0x8410
#define C_UP    0x07E8
#define C_DN    0xF8A3

TFT_eSPI tft;
TFT_eSprite tick(&tft);
SPIClass tspi(VSPI);
XPT2046_Touchscreen ts(T_CS, T_IRQ);

struct Price { int32_t hi, lo, avg1h, vol1h; };
Price P[NUM_ITEMS];          // UI copy (core 1 only)
Price Pstage[NUM_ITEMS];     // fetch task scratch (core 0 only)
SemaphoreHandle_t dataMtx; volatile bool newData = false, fetching = false, forceFetch = false; volatile bool lastOk = true;
int hr[HIGH_ROLLERS]; int64_t hrM[HIGH_ROLLERS]; int nHr = 0;
unsigned long lastFetch = 0; bool haveData = false; char updated[16] = "--:--";
int tickX = 320, tickW = 0;

String fmt(int64_t v) {
  bool n = v < 0; if (n) v = -v; char b[24];
  if (v >= 10000000) snprintf(b, 24, "%.1fM", v / 1e6);
  else if (v >= 100000) snprintf(b, 24, "%lldK", v / 1000);
  else snprintf(b, 24, "%lld", v);
  return n ? "-" + String(b) : String(b);
}
// 2% rounded down (so <50gp pays 0), capped 5M; wiki exempt items pay 0
int64_t tax(int i, int64_t sell) { if (ITEMS[i].taxFree) return 0; int64_t t = sell * 2 / 100; return t > 5000000 ? 5000000 : t; }
int64_t margin(int i, const Price& p) { return (int64_t)p.hi - tax(i, p.hi) - p.lo; }

void status(const char* s, uint16_t c = C_DIM) {
  tft.fillRect(0, 222, 320, 18, C_PANEL);
  tft.setTextColor(c, C_PANEL); tft.setTextDatum(ML_DATUM); tft.drawString(s, 4, 231, 2);
  tft.setTextDatum(MR_DATUM); tft.setTextColor(C_DIM, C_PANEL);
  String r = String(WiFi.isConnected() ? "WiFi " + String(WiFi.RSSI()) + "dBm" : "WiFi --") + "  upd " + updated;
  tft.drawString(r, 316, 231, 2);
}

// Decodes HTTP/1.1 chunked transfer-encoding on the fly so ArduinoJson can stream-parse.
// (The Wiki/Cloudflare returns 403 to HTTP/1.0 requests, so useHTTP10 cannot be used.)
class ChunkedStream : public Stream {
  Stream& s; bool chunked; long left = 0; bool done = false; unsigned long to;
  int rd() { unsigned long t = millis(); while (!s.available()) { if (millis() - t > to) return -1; delay(1); } return s.read(); }
  bool next() {
    if (!chunked) return true;
    if (left > 0) return true;
    if (done) return false;
    String l; int c;
    while ((c = rd()) >= 0 && c != '\n') if (c != '\r') l += (char)c;
    if (l.length() == 0 && c >= 0) { while ((c = rd()) >= 0 && c != '\n') if (c != '\r') l += (char)c; }
    left = strtol(l.c_str(), nullptr, 16);
    if (left <= 0) { done = true; return false; }
    return true;
  }
 public:
  ChunkedStream(Stream& st, bool ch, unsigned long timeout) : s(st), chunked(ch), to(timeout) {}
  int available() override { return (next() ? 1 : 0); }
  int read() override { if (!next()) return -1; int c = rd(); if (c >= 0 && chunked) { if (--left == 0) { rd(); rd(); } } return c; }
  int peek() override { if (!next()) return -1; unsigned long t = millis(); while (!s.available()) { if (millis() - t > to) return -1; delay(1); } return s.peek(); }
  size_t write(uint8_t) override { return 0; }
};

String lastErr;
bool fetch(const char* ep, JsonDocument& filter, JsonDocument& doc) {
  WiFiClientSecure cl; cl.setInsecure(); cl.setTimeout(20);
  HTTPClient http; http.setTimeout(20000); http.setConnectTimeout(10000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  const char* hk[] = {"Transfer-Encoding"}; 
  if (!http.begin(cl, String(API) + ep)) { lastErr = String(ep) + ": begin failed"; return false; }
  http.setUserAgent(UA); http.addHeader("Accept", "application/json");
  http.collectHeaders(hk, 1);
  unsigned long t0 = millis();
  int code = http.GET();
  Serial.printf("[http] %s -> %d (%s) heap %u\n", ep, code, code < 0 ? http.errorToString(code).c_str() : "ok", ESP.getFreeHeap());
  if (code != 200) { lastErr = String(ep) + ": HTTP " + code + (code < 0 ? " " + http.errorToString(code) : ""); http.end(); return false; }
  bool ch = http.header("Transfer-Encoding").equalsIgnoreCase("chunked");
  ChunkedStream cs(*http.getStreamPtr(), ch, 15000);
  DeserializationError e = deserializeJson(doc, cs, DeserializationOption::Filter(filter));
  http.end();
  Serial.printf("[json] %s %s chunked=%d %lums\n", ep, e.c_str(), ch, millis() - t0);
  if (e) { lastErr = String(ep) + ": JSON " + e.c_str(); return false; }
  return true;
}

bool refresh() {
  bool ok = true; lastErr = "";
  { JsonDocument f, d;
    for (int i = 0; i < NUM_ITEMS; i++) { auto o = f["data"][String(ITEMS[i].id)].to<JsonObject>(); o["high"] = true; o["low"] = true; }
    if (fetch("latest", f, d)) for (int i = 0; i < NUM_ITEMS; i++) { JsonObject o = d["data"][String(ITEMS[i].id)]; Pstage[i].hi = o["high"] | 0; Pstage[i].lo = o["low"] | 0; }
    else ok = false; }
  { JsonDocument f, d;
    for (int i = 0; i < NUM_ITEMS; i++) { auto o = f["data"][String(ITEMS[i].id)].to<JsonObject>(); o["avgHighPrice"] = true; o["highPriceVolume"] = true; o["lowPriceVolume"] = true; }
    if (fetch("1h", f, d)) for (int i = 0; i < NUM_ITEMS; i++) { JsonObject o = d["data"][String(ITEMS[i].id)]; Pstage[i].avg1h = o["avgHighPrice"] | 0; Pstage[i].vol1h = (o["highPriceVolume"] | 0) + (o["lowPriceVolume"] | 0); }
    else ok = false; }
  if (ok) { xSemaphoreTake(dataMtx, portMAX_DELAY); newData = true; struct tm t; if (getLocalTime(&t, 100)) strftime(updated, 16, "%H:%M:%S", &t); else snprintf(updated, 16, "+%lus", millis() / 1000); xSemaphoreGive(dataMtx); }
  Serial.printf("[data] refresh %s on core %d, heap %u\n", ok ? "OK" : "FAILED", xPortGetCoreID(), ESP.getFreeHeap());
  return ok;
}

void computeHighRollers() {
  nHr = 0;
  for (int i = 0; i < NUM_ITEMS; i++) {
    if (!P[i].hi || !P[i].lo || P[i].vol1h < 1) continue;
    int64_t m = margin(i, P[i]);
    int k = nHr < HIGH_ROLLERS ? nHr : HIGH_ROLLERS;
    while (k > 0 && hrM[k-1] < m) { if (k < HIGH_ROLLERS) { hr[k] = hr[k-1]; hrM[k] = hrM[k-1]; } k--; }
    if (k < HIGH_ROLLERS) { hr[k] = i; hrM[k] = m; if (nHr < HIGH_ROLLERS) nHr++; }
  }
}

void drawTicker() {
  tick.fillSprite(C_BG); int x = tickX;
  for (int j = 0; j < nHr && x < 320; j++) {
    int i = hr[j];
    float ch = P[i].avg1h ? (P[i].hi - P[i].avg1h) * 100.0f / P[i].avg1h : 0;
    String n = String(ITEMS[i].name) + " " + fmt(P[i].hi) + " m" + fmt(hrM[j]) + " ";
    char c[16]; snprintf(c, 16, "%+.1f%%   ", ch);
    int w = tick.textWidth(n, 2), w2 = tick.textWidth(c, 2);
    if (x + w + w2 > 0) {
      tick.setTextColor(C_GOLD); tick.drawString(n, x, 2, 2);
      tick.setTextColor(ch >= 0 ? C_UP : C_DN); tick.drawString(c, x + w, 2, 2);
    }
    x += w + w2;
  }
  tickW = 0; for (int j = 0; j < nHr; j++) { int i = hr[j]; char c[16]; float ch = P[i].avg1h ? (P[i].hi - P[i].avg1h) * 100.0f / P[i].avg1h : 0; snprintf(c, 16, "%+.1f%%   ", ch);
    tickW += tick.textWidth(String(ITEMS[i].name) + " " + fmt(P[i].hi) + " m" + fmt(hrM[j]) + " ", 2) + tick.textWidth(c, 2); }
  tick.pushSprite(0, 0);
}

void drawFlips() {
  int idx[5] = {-1,-1,-1,-1,-1}; int64_t mg[5] = {0};
  for (int i = 0; i < NUM_ITEMS; i++) {
    if (!P[i].hi || !P[i].lo || P[i].vol1h < VOL_FLOOR || P[i].lo > MAX_BUY) continue;
    int64_t m = margin(i, P[i]);
    if (m <= 0 || m * 100.0f / P[i].lo < ROI_FLOOR) continue;
    for (int k = 0; k < 5; k++) if (idx[k] < 0 || P[i].vol1h > P[idx[k]].vol1h) {
      for (int j = 4; j > k; j--) { idx[j] = idx[j-1]; mg[j] = mg[j-1]; }
      idx[k] = i; mg[k] = m; break; }
  }
  tft.fillRect(0, 20, 320, 202, C_BG);
  tft.setTextDatum(TL_DATUM); tft.setTextColor(C_GOLD, C_BG); tft.drawString("GE Tracker", 6, 24, 4);
  tft.setTextColor(C_DIM, C_BG); tft.drawString("Top flips by volume", 160, 30, 2);
  int y = 52; tft.fillRect(0, y, 320, 16, C_PANEL); tft.setTextColor(C_DIM, C_PANEL);
  tft.drawString("Item", 4, y, 2); tft.setTextDatum(TR_DATUM);
  tft.drawString("Buy", 180, y, 2); tft.drawString("Sell", 226, y, 2); tft.drawString("Margin", 270, y, 2); tft.drawString("Vol/h", 316, y, 2);
  y += 20;
  for (int k = 0; k < 5; k++) {
    if (idx[k] < 0) break; int i = idx[k];
    uint16_t bg = (k & 1) ? C_PANEL : C_BG;
    tft.fillRect(0, y - 2, 320, 30, bg);
    tft.setTextDatum(TL_DATUM); tft.setTextColor(C_TXT, bg);
    String n = ITEMS[i].name; while (tft.textWidth(n, 2) > 132) n.remove(n.length() - 1);
    tft.drawString(n, 4, y, 2);
    char r[12]; snprintf(r, 12, "%.1f%%", mg[k] * 100.0 / P[i].lo);
    tft.setTextColor(C_DIM, bg); tft.drawString("ROI " + String(r) + "  lim " + fmt(ITEMS[i].limit) + (ITEMS[i].taxFree ? "  tax-free" : ""), 4, y + 14, 1);
    tft.setTextDatum(TR_DATUM); tft.setTextColor(C_TXT, bg);
    tft.drawString(fmt(P[i].lo), 180, y + 4, 2); tft.drawString(fmt(P[i].hi), 226, y + 4, 2);
    tft.setTextColor(mg[k] > 0 ? C_UP : C_DN, bg); tft.drawString(fmt(mg[k]), 270, y + 4, 2);
    tft.setTextColor(C_TXT, bg); tft.drawString(fmt(P[i].vol1h), 316, y + 4, 2);
    Serial.printf("[flip] %d. %s buy %d sell %d margin %lld vol1h %d\n", k + 1, ITEMS[i].name, P[i].lo, P[i].hi, mg[k], P[i].vol1h);
    y += 30;
  }
  if (idx[0] < 0) { tft.setTextDatum(MC_DATUM); tft.setTextColor(C_DIM, C_BG); tft.drawString(haveData ? "No flips match filters" : "No data yet", 160, 140, 2); }
}

void apCallback(WiFiManager* m) {
  Serial.println("[wifi] SETUP MODE: AP 'GE-Tracker-Setup' at 192.168.4.1");
  tft.fillScreen(C_BG); tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_GOLD, C_BG); tft.drawString("GE Tracker - WiFi setup", 10, 10, 4);
  tft.setTextColor(C_TXT, C_BG);
  const char* L[] = {"1. On your phone, open WiFi settings", "2. Join network:  GE-Tracker-Setup", "3. A setup page should pop up", "   (if not, browse to 192.168.4.1)", "4. Tap 'Configure WiFi', pick your", "   network, enter password, Save", "", "The tracker restarts and connects."};
  for (int i = 0; i < 8; i++) tft.drawString(L[i], 10, 50 + i * 20, 2);
}

void fetchTask(void*) {
  unsigned long last = 0; bool first = true;
  for (;;) {
    if (first || forceFetch || millis() - last > REFRESH_MS) {
      first = false; forceFetch = false; fetching = true;
      if (!WiFi.isConnected()) WiFi.reconnect();
      xSemaphoreTake(dataMtx, portMAX_DELAY); memcpy(Pstage, P, sizeof(P)); xSemaphoreGive(dataMtx); // keep old values if one endpoint fails
      lastOk = refresh(); last = millis(); fetching = false;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void setup() {
  Serial.begin(115200); delay(200);
  Serial.printf("\n[boot] GE Tracker starting, %d items, heap %u\n", NUM_ITEMS, ESP.getFreeHeap());
  tft.init(); tft.setRotation(1); tft.fillScreen(C_BG);
  pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH);
  Serial.printf("[tft] init OK %dx%d\n", tft.width(), tft.height());
  tick.setColorDepth(16); tick.createSprite(320, 20); tick.setTextDatum(TL_DATUM);
  tspi.begin(T_CLK, T_MISO, T_MOSI, T_CS); ts.begin(tspi); ts.setRotation(1);
  tft.setTextDatum(MC_DATUM); tft.setTextColor(C_GOLD, C_BG); tft.drawString("GE Tracker", 160, 100, 4);
  tft.setTextColor(C_DIM, C_BG); tft.drawString("Connecting to WiFi...", 160, 135, 2);
  WiFiManager wm; wm.setAPCallback(apCallback); wm.setConnectTimeout(20);
  wm.setTitle("GE Tracker");
  // hold screen touched at boot to reset WiFi credentials
  if (ts.touched()) { Serial.println("[wifi] touch at boot -> reset settings"); wm.resetSettings(); }
  if (!wm.autoConnect("GE-Tracker-Setup")) { Serial.println("[wifi] portal failed, restarting"); delay(2000); ESP.restart(); }
  Serial.printf("[wifi] connected %s ip %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  configTzTime("EST5EDT,M3.2.0,M11.1.0", "pool.ntp.org");
  tft.fillScreen(C_BG); drawFlips(); status("Fetching prices...", C_GOLD);
  dataMtx = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(fetchTask, "fetch", 16384, nullptr, 1, nullptr, 0); // network on core 0, TFT stays on core 1
}

void loop() {
  static unsigned long lt = 0, hb = 0; static uint32_t frames = 0;
  if (millis() - lt >= TICKER_FRAME_MS) { lt = millis(); tickX -= TICKER_STEP_PX; if (tickW && tickX < -tickW) tickX = 320; drawTicker(); frames++; }
  if (newData && xSemaphoreTake(dataMtx, 0) == pdTRUE) {
    memcpy(P, Pstage, sizeof(P)); newData = false; xSemaphoreGive(dataMtx);
    haveData = true; computeHighRollers(); drawFlips(); status("Tap screen to refresh");
    Serial.printf("[ui] new data applied, %d high rollers\n", nHr);
  }
  static bool wasFetching = false;
  if (fetching != wasFetching) { wasFetching = fetching; if (fetching) status("Updating...", C_GOLD); else if (!lastOk) status(("ERR " + lastErr).c_str(), C_DN); }
  if (millis() - hb > 2000) { hb = millis(); if (fetching) Serial.printf("[loop] core %d alive during fetch, frames=%u\n", xPortGetCoreID(), frames); }
  if (ts.touched()) { static unsigned long lastTap = 0; if (millis() - lastTap > 1500) { lastTap = millis(); forceFetch = true; Serial.println("[touch] refresh"); } }
}
