/*
 * ============================================================
 *  Swarm Dashboard v1.3
 *  Target: HOSYOND 4.0" ESP32 Display (ST7796S, 480x320, SPI)
 * ============================================================
 *
 *  WHAT IT DOES:
 *    - Polls Bitaxe / NerdAxe (AxeOS HTTP) and Avalon Nano 3s
 *      (CGMiner TCP) miners on your LAN
 *    - Shows up to 12 miners on screen
 *    - Web UI on port 80 for managing miners
 *    - WiFi captive portal on first boot
 *
 *  REQUIRED LIBRARIES (Arduino IDE -> Tools -> Manage Libraries):
 *    - TFT_eSPI       by Bodmer
 *    - ArduinoJson    by Benoit Blanchon
 *    - WiFiManager    by tzapu
 *
 *  IMPORTANT: Use ESP32 board package version 2.0.17, not 3.x
 *  (ESP32 3.x has incompatibilities with WiFiManager)
 *
 *  BEFORE COMPILING:
 *    Open the TFT_eSPI library folder, replace User_Setup.h with
 *    the contents of User_Setup_snippet.h from this project.
 *
 *  ARDUINO IDE BOARD SETTINGS:
 *    Board:             ESP32 Dev Module
 *    Flash Size:        4MB (32Mb)
 *    Partition Scheme:  Default 4MB with spiffs
 *    PSRAM:             Disabled
 *    Upload Speed:      921600
 *
 *  FIRST-TIME WIFI SETUP:
 *    1. Power on the board
 *    2. On your phone, join WiFi: SwarmDashboard-Setup
 *       (password: swarm1234)
 *    3. Captive portal opens. Pick your home WiFi, enter password
 *    4. Board reboots and joins your home WiFi
 *
 *  MANAGING MINERS:
 *    Browse to the dashboard's IP from any device on your network.
 *
 * ============================================================
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <FS.h>
#include <WiFiManager.h>

#include "config.h"
#include "MinerStore.h"
#include "AvalonAPI.h"
#include "WebUI.h"

// ============================================================
//  GLOBALS
// ============================================================
TFT_eSPI    tft = TFT_eSPI();
MinerStore  store;
WebUI       webui(store);

// ----- AP mode for first-time setup -----
#define AP_SSID      "SwarmDashboard-Setup"
#define AP_PASSWORD  "swarm1234"
#define AP_TIMEOUT_S 180

// ----- Color palette (RGB565) -----
#define COL_BG        0x0000  // black
#define COL_BORDER    0x04FF  // cyan
#define COL_HEADER    0x04FF  // cyan
#define COL_LABEL     0xFFFF  // white
#define COL_GREEN     0x07E0
#define COL_YELLOW    0xFFE0
#define COL_RED       0xF800
#define COL_ORANGE    0xFCA0
#define COL_MAGENTA   0xF81F
#define COL_PURPLE    0xA01F
#define COL_GREY      0x7BEF
#define COL_DARKGREY  0x39E7

inline uint16_t colorForType(uint8_t t) {
  switch (t) {
    case TYPE_BITAXE:  return COL_ORANGE;
    case TYPE_NERDAXE: return COL_MAGENTA;
    case TYPE_AVALON:  return COL_GREEN;
    default:           return COL_LABEL;
  }
}

// ----- Runtime data -----

// Rolling average of recent Avalon hashrate readings.
// One independent buffer per miner index, so multiple Avalons stay isolated.
// Buffer holds up to AVALON_AVG_SAMPLES recent samples; we report the mean.
#define AVALON_AVG_SAMPLES 8

struct AvalonAvg {
  float    samples[AVALON_AVG_SAMPLES] = {0};
  uint8_t  count = 0;     // how many slots are filled
  uint8_t  next  = 0;     // index where the next sample goes (ring buffer)

  void reset() { count = 0; next = 0; }

  void push(float v) {
    samples[next] = v;
    next = (next + 1) % AVALON_AVG_SAMPLES;
    if (count < AVALON_AVG_SAMPLES) count++;
  }

  float mean() const {
    if (count == 0) return 0;
    float s = 0;
    for (uint8_t i = 0; i < count; i++) s += samples[i];
    return s / count;
  }
};

AvalonAvg avalonAvg[MAX_MINERS];

struct MinerData {
  bool     online   = false;
  float    hashrate = 0;
  uint16_t power    = 0;
  float    temp     = 0;
  float    bestDiff = 0;
  uint32_t uptime   = 0;
  char     poolUrl[64] = {0};   // each miner's pool URL (used to find most common)
};
MinerData runtime[MAX_MINERS];

struct PoolInfo {
  String   url;
  String   worker;
  float    poolHashrate = 0;
  uint32_t accepted     = 0;
  uint32_t rejected     = 0;
} pool;

struct SysInfo {
  float    esp32Temp = 0;
  uint32_t uptime    = 0;
  uint32_t freeHeap  = 0;
  int8_t   wifiRssi  = 0;
} sys;

unsigned long lastPoll      = 0;
unsigned long bootMillis    = 0;
volatile bool pollRequested = false;

// Toggles each completed poll wave on big screens.
// false = show UP column with uptime, true = show J/Ts column with efficiency.
bool showJTs = false;

// ----- Layout (480x320 landscape) -----
const int SCREEN_W = 480;
const int SCREEN_H = 320;
const int HDR_H    = 24;

const int TBL_X = 4,    TBL_Y = 28;
const int TBL_W = 320,  TBL_H = 290;
const int ROW_H = 21,   HEADER_ROW_H = 18;

const int RIGHT_X = 328, RIGHT_W = 148;
const int POOL_Y  = 28,  POOL_H  = 200;
const int SYS_Y   = 232, SYS_H   = 86;

const int COL_DEV  = TBL_X + 6;
const int COL_THS  = TBL_X + 70;
const int COL_PWR  = TBL_X + 120;
const int COL_TEMP = TBL_X + 170;
const int COL_DIFF = TBL_X + 220;
const int COL_UP   = TBL_X + 285;

// ============================================================
//  CALLBACKS for WebUI
// ============================================================
void requestPollNow()         { pollRequested = true; }
uint32_t getBootMillis()      { return bootMillis; }
uint32_t getFreeHeap()        { return ESP.getFreeHeap(); }
int8_t   getRssi()            { return WiFi.RSSI(); }
float    getEspTemp() {
#ifdef CONFIG_IDF_TARGET_ESP32
  return temperatureRead();
#else
  return 0.0f;
#endif
}
float    getPoolHashrateTHs() { return pool.poolHashrate / 1000.0f; }
String   getPoolUrl()         { return pool.url; }
String   getPoolWorker()      { return pool.worker; }
uint32_t getPoolAccepted()    { return pool.accepted; }
uint32_t getPoolRejected()    { return pool.rejected; }

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Swarm Dashboard v1.3 ===");

  bootMillis = millis();

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(COL_BG);

  store.begin();
  Serial.printf("Loaded %u miners from storage\n", store.count());

  startWiFi();

  webui.begin();
  Serial.print("Web UI: http://");
  Serial.println(WiFi.localIP());

  drawStaticUI();
  pollAllMiners();
  renderAll();
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
  webui.loop();

  unsigned long now    = millis();
  uint32_t      pollMs = webui.getPollInterval();

  if (pollRequested || (now - lastPoll >= pollMs)) {
    pollRequested = false;
    lastPoll      = now;
    pollAllMiners();
  }

  static unsigned long lastClock = 0;
  if (now - lastClock >= 1000) {
    lastClock = now;
    drawSysUptime();
  }

  delay(5);
}

// ============================================================
//  WIFI SETUP
// ============================================================
void startWiFi() {
  WiFiManager wm;

#ifdef CLEAR_WIFI_ON_BOOT
  wm.resetSettings();
  Serial.println("WiFi creds wiped (CLEAR_WIFI_ON_BOOT)");
#endif

  drawAPSplash();

  wm.setConfigPortalTimeout(AP_TIMEOUT_S);
  wm.setTitle("Swarm Dashboard");
  wm.setClass("invert");

  bool connected = wm.autoConnect(AP_SSID, AP_PASSWORD);
  if (!connected) {
    drawSplash("WiFi setup timed out. Restarting...");
    delay(2000);
    ESP.restart();
  }

  Serial.print("Connected. IP: ");
  Serial.println(WiFi.localIP());
  drawSplash(("Connected: " + WiFi.localIP().toString()).c_str());
  delay(900);
}

void drawAPSplash() {
  tft.fillScreen(COL_BG);
  tft.setTextDatum(MC_DATUM);

  tft.setTextColor(COL_HEADER, COL_BG);
  tft.drawString("Swarm Dashboard", SCREEN_W/2, 50, 4);

  tft.setTextColor(COL_LABEL, COL_BG);
  tft.drawString("First-time WiFi setup", SCREEN_W/2, 95, 2);

  tft.drawRoundRect(40, 120, SCREEN_W - 80, 130, 6, COL_BORDER);

  tft.setTextColor(COL_YELLOW, COL_BG);
  tft.drawString("1. On your phone, join WiFi:", SCREEN_W/2, 140, 2);
  tft.setTextColor(COL_GREEN, COL_BG);
  tft.drawString(AP_SSID, SCREEN_W/2, 162, 4);

  tft.setTextColor(COL_YELLOW, COL_BG);
  tft.drawString("2. Password:", SCREEN_W/2, 192, 2);
  tft.setTextColor(COL_GREEN, COL_BG);
  tft.drawString(AP_PASSWORD, SCREEN_W/2, 212, 4);

  tft.setTextColor(COL_LABEL, COL_BG);
  tft.drawString("Captive portal will open automatically.", SCREEN_W/2, 270, 2);
  tft.setTextColor(COL_GREY, COL_BG);
  tft.drawString("(or browse to 192.168.4.1)", SCREEN_W/2, 290, 2);

  tft.setTextDatum(TL_DATUM);
}

// ============================================================
//  POLLING (one-at-a-time wave with purple highlight)
// ============================================================

// Find the pool URL that appears most often across online miners.
// Tie-breaker: first one encountered.
String mostCommonPoolUrl() {
  uint8_t n = store.count();
  String best = "";
  uint8_t bestCount = 0;

  for (uint8_t i = 0; i < n; i++) {
    if (!runtime[i].online || runtime[i].poolUrl[0] == 0) continue;

    String thisUrl = String(runtime[i].poolUrl);
    uint8_t count = 0;
    for (uint8_t j = 0; j < n; j++) {
      if (runtime[j].online && String(runtime[j].poolUrl) == thisUrl) {
        count++;
      }
    }
    if (count > bestCount) {
      bestCount = count;
      best = thisUrl;
    }
  }
  return best;
}

void pollAllMiners() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    return;
  }

  uint8_t n     = store.count();
  uint8_t shown = (n > DASHBOARD_ROWS) ? DASHBOARD_ROWS : n;

  // Reset pool counters at start of cycle
  pool.accepted = 0;
  pool.rejected = 0;

  for (uint8_t i = 0; i < n; i++) {
    // Highlight the row about to be polled (visible only if on screen)
    if (i < shown) {
      highlightMinerRow(i);
    }

    // Do the actual poll
    const MinerEntry& e = store.get(i);
    bool ok = false;
    switch (e.type) {
      case TYPE_BITAXE:
      case TYPE_NERDAXE:
        ok = pollAxeOS(i, e);
        break;
      case TYPE_AVALON:
        ok = pollAvalon(i, e);
        break;
    }
    (void) ok;

    // Redraw row with fresh data, then re-stamp name in purple so it stays
    // highlighted for the whole pause period
    if (i < shown) {
      int y = TBL_Y + HEADER_ROW_H + 4 + i * ROW_H;
      drawMinerRow(i, y);
      // Re-paint the name in purple over the freshly-drawn row
      tft.fillRect(COL_DEV - 2, y, (COL_THS - COL_DEV) - 2, ROW_H - 2, COL_BG);
      tft.setTextColor(COL_PURPLE, COL_BG);
      tft.drawString(e.name, COL_DEV, y, 2);
    }

    // Keep the web UI responsive and pace the wave (~3s per miner)
    unsigned long pauseUntil = millis() + 3000;
    while (millis() < pauseUntil) {
      webui.loop();
      delay(10);
    }

    // Restore the miner name to its proper color before moving to the next
    if (i < shown) {
      int y = TBL_Y + HEADER_ROW_H + 4 + i * ROW_H;
      tft.fillRect(COL_DEV - 2, y, (COL_THS - COL_DEV) - 2, ROW_H - 2, COL_BG);
      tft.setTextColor(colorForType(e.type), COL_BG);
      tft.drawString(e.name, COL_DEV, y, 2);
    }
  }

  // Recompute pool hashrate (sum of online miners)
  // and find most-common pool URL across all online miners
  float poolHashSum = 0;
  for (uint8_t i = 0; i < n; i++) {
    if (runtime[i].online) {
      poolHashSum += runtime[i].hashrate;
    }
  }
  pool.poolHashrate = poolHashSum;
  pool.url = mostCommonPoolUrl();

  // Update TOTAL row, pool panel, and system info
  drawTotalRow();
  drawPoolPanel();

  sys.uptime    = (millis() - bootMillis) / 1000;
  sys.freeHeap  = ESP.getFreeHeap();
  sys.wifiRssi  = WiFi.RSSI();
  sys.esp32Temp = getEspTemp();
  drawSysPanel();
  drawHeaderInfo();

  // Flip UP <-> J/Ts column for next wave, then redraw the
  // header label and every visible row's right-most column to match.
  showJTs = !showJTs;
  drawUpJTsHeader();
  uint8_t shownRows = (n > DASHBOARD_ROWS) ? DASHBOARD_ROWS : n;
  for (uint8_t i = 0; i < shownRows; i++) {
    int y = TBL_Y + HEADER_ROW_H + 4 + i * ROW_H;
    drawMinerRow(i, y);
  }
}

// Paint the miner's name in purple to show it's being polled
void highlightMinerRow(uint8_t i) {
  int y = TBL_Y + HEADER_ROW_H + 4 + i * ROW_H;
  const MinerEntry& e = store.get(i);
  // Just clear the device-name area and redraw it in purple
  tft.fillRect(COL_DEV - 2, y, (COL_THS - COL_DEV) - 2, ROW_H - 2, COL_BG);
  tft.setTextColor(COL_PURPLE, COL_BG);
  tft.drawString(e.name, COL_DEV, y, 2);
}

bool pollAxeOS(uint8_t idx, const MinerEntry& e) {
  HTTPClient http;
  String url = String("http://") + e.ip + "/api/system/info";
  http.setTimeout(2500);
  http.begin(url);
  int code = http.GET();

  if (code != 200) {
    runtime[idx].online = false;
    http.end();
    return false;
  }

  StaticJsonDocument<3072> doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    runtime[idx].online = false;
    return false;
  }

  runtime[idx].online   = true;
  runtime[idx].hashrate = doc["hashRate"] | 0.0f;
  runtime[idx].power    = (uint16_t)(doc["power"] | 0.0f);
  runtime[idx].temp     = doc["temp"] | 0.0f;
  runtime[idx].bestDiff = doc["bestSessionDiff"] | doc["bestDiff"] | 0.0f;
  runtime[idx].uptime   = doc["uptimeSeconds"] | 0;

  // Per-miner pool URL (used to compute most common pool)
  String purl = String((const char*)(doc["stratumURL"] | "")) + ":" +
                String((int)(doc["stratumPort"] | 0));
  strlcpy(runtime[idx].poolUrl, purl.c_str(), sizeof(runtime[idx].poolUrl));

  pool.accepted += (uint32_t)(doc["sharesAccepted"] | 0);
  pool.rejected += (uint32_t)(doc["sharesRejected"] | 0);
  return true;
}

bool pollAvalon(uint8_t idx, const MinerEntry& e) {
  AvalonReading r;
  if (!AvalonAPI::fetch(e.ip, r)) {
    runtime[idx].online = false;
    avalonAvg[idx].reset();   // wipe history when offline
    return false;
  }

  // Feed raw 5s reading into the rolling average buffer for this miner
  avalonAvg[idx].push(r.hashrate);

  runtime[idx].online   = true;
  runtime[idx].hashrate = avalonAvg[idx].mean();   // smoothed value
  runtime[idx].power    = r.power;
  runtime[idx].temp     = r.temp;
  runtime[idx].bestDiff = r.bestDiff;
  runtime[idx].uptime   = r.uptime;

  strlcpy(runtime[idx].poolUrl, r.poolUrl, sizeof(runtime[idx].poolUrl));

  pool.accepted += r.accepted;
  pool.rejected += r.rejected;
  return true;
}

// ============================================================
//  DRAWING
// ============================================================
void drawSplash(const char* msg) {
  tft.fillScreen(COL_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_HEADER, COL_BG);
  tft.drawString("Swarm Dashboard", SCREEN_W/2, SCREEN_H/2 - 20, 4);
  tft.setTextColor(COL_LABEL, COL_BG);
  tft.drawString(msg, SCREEN_W/2, SCREEN_H/2 + 16, 2);
  tft.setTextDatum(TL_DATUM);
}

void drawStaticUI() {
  tft.fillScreen(COL_BG);

  tft.drawRoundRect(0, 0, SCREEN_W, HDR_H + 2, 4, COL_BORDER);
  tft.setTextColor(COL_HEADER, COL_BG);
  tft.drawString("Swarm Dashboard", 28, 4, 4);
  tft.drawRect(6, 4, 16, 16, COL_HEADER);
  tft.drawRect(9, 7, 10, 10, COL_HEADER);

  tft.drawRoundRect(TBL_X, TBL_Y, TBL_W, TBL_H, 4, COL_BORDER);

  tft.setTextColor(COL_LABEL, COL_BG);
  tft.drawString("DEVICE", COL_DEV,  TBL_Y + 3, 2);
  tft.drawString("TH/S",   COL_THS,  TBL_Y + 3, 2);
  tft.drawString("PWR",    COL_PWR,  TBL_Y + 3, 2);
  tft.drawString("TEMP",   COL_TEMP, TBL_Y + 3, 2);
  tft.drawString("DIFF",   COL_DIFF, TBL_Y + 3, 2);
  drawUpJTsHeader();   // draws "UP" or "J/Ts" based on showJTs flag
  tft.drawFastHLine(TBL_X + 2, TBL_Y + HEADER_ROW_H, TBL_W - 4, COL_DARKGREY);

  tft.drawRoundRect(RIGHT_X, POOL_Y, RIGHT_W, POOL_H, 4, COL_BORDER);
  tft.setTextColor(COL_HEADER, COL_BG);
  tft.drawString("POOL INFO", RIGHT_X + 6, POOL_Y + 3, 2);

  tft.drawRoundRect(RIGHT_X, SYS_Y, RIGHT_W, SYS_H, 4, COL_BORDER);
  tft.setTextColor(COL_HEADER, COL_BG);
  tft.drawString("SYSTEM INFO", RIGHT_X + 6, SYS_Y + 3, 2);

  drawWifiIcon(SCREEN_W - 130, 4);
}

void drawWifiIcon(int x, int y) {
  tft.fillCircle(x + 6, y + 12, 2, COL_HEADER);
  for (int r = 5; r <= 11; r += 3) {
    for (int a = 220; a <= 320; a += 5) {
      float rad = a * 3.14159 / 180.0;
      int px = x + 6 + r * cos(rad);
      int py = y + 12 + r * sin(rad);
      tft.drawPixel(px, py, COL_HEADER);
    }
  }
}

void renderAll() {
  drawMinerTable();
  drawPoolPanel();
  drawSysPanel();
  drawHeaderInfo();
}

void drawMinerTable() {
  tft.fillRect(TBL_X + 1, TBL_Y + HEADER_ROW_H + 2,
               TBL_W - 2, TBL_H - HEADER_ROW_H - 4, COL_BG);

  uint8_t n     = store.count();
  uint8_t shown = (n > DASHBOARD_ROWS) ? DASHBOARD_ROWS : n;

  if (n == 0) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(COL_GREY, COL_BG);
    tft.drawString("No miners configured.",
                   TBL_X + TBL_W/2, TBL_Y + HEADER_ROW_H + 60, 2);
    tft.setTextColor(COL_LABEL, COL_BG);
    tft.drawString("Visit:",
                   TBL_X + TBL_W/2, TBL_Y + HEADER_ROW_H + 100, 2);
    String ip = "http://" + WiFi.localIP().toString() + "/";
    tft.setTextColor(COL_GREEN, COL_BG);
    tft.drawString(ip, TBL_X + TBL_W/2, TBL_Y + HEADER_ROW_H + 122, 4);
    tft.setTextColor(COL_LABEL, COL_BG);
    tft.drawString("from any phone or PC on",
                   TBL_X + TBL_W/2, TBL_Y + HEADER_ROW_H + 156, 2);
    tft.drawString("your network to add miners.",
                   TBL_X + TBL_W/2, TBL_Y + HEADER_ROW_H + 176, 2);
    tft.setTextDatum(TL_DATUM);
    return;
  }

  for (uint8_t i = 0; i < shown; i++) {
    int y = TBL_Y + HEADER_ROW_H + 4 + i * ROW_H;
    drawMinerRow(i, y);
  }

  drawTotalRow();
}

void drawTotalRow() {
  uint8_t n     = store.count();
  uint8_t shown = (n > DASHBOARD_ROWS) ? DASHBOARD_ROWS : n;
  if (n == 0) return;

  int totalY = TBL_Y + HEADER_ROW_H + 4 + shown * ROW_H + 2;

  // Clear the entire total row area first
  tft.fillRect(TBL_X + 1, totalY - 1, TBL_W - 2, ROW_H, COL_BG);
  tft.drawFastHLine(TBL_X + 2, totalY - 2, TBL_W - 4, COL_DARKGREY);

  tft.setTextColor(COL_HEADER, COL_BG);
  tft.drawString("TOTAL", COL_DEV, totalY + 2, 2);

  char buf[32];
  snprintf(buf, sizeof(buf), "%.2f TH/S", pool.poolHashrate / 1000.0);
  tft.drawString(buf, COL_THS, totalY + 2, 2);

  uint32_t totalPwr = 0;
  for (uint8_t i = 0; i < n; i++) {
    if (runtime[i].online) totalPwr += runtime[i].power;
  }
  tft.setTextColor(COL_YELLOW, COL_BG);
  snprintf(buf, sizeof(buf), "Total Pwr: %luW", (unsigned long)totalPwr);
  tft.drawString(buf, COL_TEMP, totalY + 2, 2);

  if (n > shown) {
    tft.setTextColor(COL_GREY, COL_BG);
    char more[16];
    snprintf(more, sizeof(more), "+%u more", n - shown);
    tft.drawString(more, COL_UP, totalY + 2, 2);
  }
}

// Redraws just the UP/J/Ts header cell. Called whenever showJTs toggles
// or when drawing the static UI. Erases the cell first to prevent text overlap.
void drawUpJTsHeader() {
  // Cell extends from COL_UP to right edge of table.
  int cellX = COL_UP - 2;
  int cellY = TBL_Y + 1;
  int cellW = (TBL_X + TBL_W) - cellX - 2;
  int cellH = HEADER_ROW_H - 2;
  tft.fillRect(cellX, cellY, cellW, cellH, COL_BG);

  tft.setTextColor(COL_LABEL, COL_BG);
  tft.drawString(showJTs ? "J/Ts" : "UP", COL_UP, TBL_Y + 3, 2);
}

void drawMinerRow(uint8_t i, int y) {
  tft.fillRect(TBL_X + 1, y, TBL_W - 2, ROW_H - 2, COL_BG);
  char buf[16];
  const MinerEntry& e = store.get(i);

  uint16_t nameColor = colorForType(e.type);
  tft.setTextColor(nameColor, COL_BG);
  tft.drawString(e.name, COL_DEV, y, 2);

  if (!runtime[i].online) {
    tft.setTextColor(COL_GREY, COL_BG);
    tft.drawString("--", COL_THS,  y, 2);
    tft.drawString("--", COL_PWR,  y, 2);
    tft.drawString("--", COL_TEMP, y, 2);
    tft.drawString("--", COL_DIFF, y, 2);
    tft.drawString("--", COL_UP,   y, 2);
    return;
  }

  tft.setTextColor(nameColor, COL_BG);
  snprintf(buf, sizeof(buf), "%.2f", runtime[i].hashrate / 1000.0);
  tft.drawString(buf, COL_THS, y, 2);

  tft.setTextColor(COL_YELLOW, COL_BG);
  snprintf(buf, sizeof(buf), "%uW", runtime[i].power);
  tft.drawString(buf, COL_PWR, y, 2);

  // Temp color: <60 green, 60-69.9 yellow, 70+ red
  uint16_t tColor = COL_GREEN;
  if (runtime[i].temp >= 70)      tColor = COL_RED;
  else if (runtime[i].temp >= 60) tColor = COL_YELLOW;
  tft.setTextColor(tColor, COL_BG);
  if (runtime[i].temp > 0) snprintf(buf, sizeof(buf), "%.0fC", runtime[i].temp);
  else                     snprintf(buf, sizeof(buf), "--");
  tft.drawString(buf, COL_TEMP, y, 2);

  tft.setTextColor(COL_YELLOW, COL_BG);
  if (runtime[i].bestDiff > 0) formatDiff(runtime[i].bestDiff, buf, sizeof(buf));
  else                         snprintf(buf, sizeof(buf), "--");
  tft.drawString(buf, COL_DIFF, y, 2);

  tft.setTextColor(COL_LABEL, COL_BG);
  if (showJTs) {
    // J/Ts = watts per terahash per second. Hashrate is stored in GH/s.
    // Convert: watts / (GH/s / 1000) = watts / TH/s
    if (runtime[i].hashrate > 0 && runtime[i].power > 0) {
      float ths = runtime[i].hashrate / 1000.0f;
      float jts = (float) runtime[i].power / ths;
      // Format: under 100 -> X.X, 100+ -> XXX (whole numbers only)
      if (jts < 100.0f) snprintf(buf, sizeof(buf), "%.1f", jts);
      else              snprintf(buf, sizeof(buf), "%.0f", jts);
    } else {
      snprintf(buf, sizeof(buf), "--");
    }
  } else {
    formatUptimeShort(runtime[i].uptime, buf, sizeof(buf));
  }
  tft.drawString(buf, COL_UP, y, 2);
}

void drawPoolPanel() {
  tft.fillRect(RIGHT_X + 1, POOL_Y + 18, RIGHT_W - 2, POOL_H - 20, COL_BG);
  int y = POOL_Y + 22;
  const int LH = 11;
  char buf[24];

  tft.setTextColor(COL_MAGENTA, COL_BG);
  tft.drawString("Pool URL:", RIGHT_X + 6, y, 1); y += LH;
  tft.setTextColor(COL_LABEL, COL_BG);
  String purl = pool.url.length() > 0 ? pool.url : String("--");
  drawWrapped(purl, RIGHT_X + 6, y, RIGHT_W - 12, 1, &y, LH);

  y += 6;
  tft.setTextColor(COL_MAGENTA, COL_BG);
  tft.drawString("Pool Hashrate:", RIGHT_X + 6, y, 1); y += LH;
  tft.setTextColor(COL_LABEL, COL_BG);
  snprintf(buf, sizeof(buf), "%.2f TH/s", pool.poolHashrate / 1000.0);
  tft.drawString(buf, RIGHT_X + 6, y, 1);
  y += LH + 4;

  tft.setTextColor(COL_GREEN, COL_BG);
  tft.drawString("Accepted:", RIGHT_X + 6, y, 1); y += LH;
  tft.setTextColor(COL_LABEL, COL_BG);
  uint32_t total = pool.accepted + pool.rejected;
  float pctA = total > 0 ? (100.0 * pool.accepted / total) : 0;
  snprintf(buf, sizeof(buf), "%lu (%.2f%%)", (unsigned long)pool.accepted, pctA);
  tft.drawString(buf, RIGHT_X + 6, y, 1);
  y += LH + 4;

  tft.setTextColor(COL_RED, COL_BG);
  tft.drawString("Rejected:", RIGHT_X + 6, y, 1); y += LH;
  tft.setTextColor(COL_LABEL, COL_BG);
  float pctR = total > 0 ? (100.0 * pool.rejected / total) : 0;
  snprintf(buf, sizeof(buf), "%lu (%.2f%%)", (unsigned long)pool.rejected, pctR);
  tft.drawString(buf, RIGHT_X + 6, y, 1);
}

void drawSysPanel() {
  tft.fillRect(RIGHT_X + 1, SYS_Y + 18, RIGHT_W - 2, SYS_H - 20, COL_BG);
  int y = SYS_Y + 22;
  const int LH = 13;
  char buf[24];

  tft.setTextColor(COL_HEADER, COL_BG);
  tft.drawString("ESP32:", RIGHT_X + 6, y, 1);
  tft.setTextColor(COL_GREEN, COL_BG);
  snprintf(buf, sizeof(buf), "%.1fC", sys.esp32Temp);
  tft.drawString(buf, RIGHT_X + RIGHT_W - 50, y, 1);
  y += LH;

  tft.setTextColor(COL_HEADER, COL_BG);
  tft.drawString("Uptime:", RIGHT_X + 6, y, 1);
  tft.setTextColor(COL_LABEL, COL_BG);
  formatUptimeShort(sys.uptime, buf, sizeof(buf));
  tft.drawString(buf, RIGHT_X + RIGHT_W - 50, y, 1);
  y += LH;

  tft.setTextColor(COL_HEADER, COL_BG);
  tft.drawString("Free Heap:", RIGHT_X + 6, y, 1);
  tft.setTextColor(COL_LABEL, COL_BG);
  snprintf(buf, sizeof(buf), "%lu KB", (unsigned long)(sys.freeHeap / 1024));
  tft.drawString(buf, RIGHT_X + RIGHT_W - 50, y, 1);
  y += LH;

  tft.setTextColor(COL_HEADER, COL_BG);
  tft.drawString("WiFi RSSI:", RIGHT_X + 6, y, 1);
  tft.setTextColor(COL_LABEL, COL_BG);
  snprintf(buf, sizeof(buf), "%d dBm", sys.wifiRssi);
  tft.drawString(buf, RIGHT_X + RIGHT_W - 50, y, 1);
}

void drawHeaderInfo() {
  // Clear right side of header bar
  tft.fillRect(SCREEN_W - 180, 2, 178, 22, COL_BG);
  drawWifiIcon(SCREEN_W - 180, 4);
  tft.setTextColor(COL_HEADER, COL_BG);
  String ip = WiFi.localIP().toString();
  tft.drawString(ip, SCREEN_W - 160, 4, 2);
}

void drawClock() {
  uint32_t s   = (millis() - bootMillis) / 1000;
  uint32_t h   = s / 3600;
  uint32_t m   = (s % 3600) / 60;
  uint32_t sec = s % 60;
  char buf[12];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu",
           (unsigned long)h, (unsigned long)m, (unsigned long)sec);
  tft.fillRect(SCREEN_W - 60, 4, 56, 16, COL_BG);
  tft.setTextColor(COL_HEADER, COL_BG);
  tft.drawString(buf, SCREEN_W - 60, 4, 2);
}

void drawSysUptime() {
  sys.uptime = (millis() - bootMillis) / 1000;
  int y = SYS_Y + 22 + 13;
  tft.fillRect(RIGHT_X + RIGHT_W - 52, y, 50, 12, COL_BG);
  char buf[20];
  formatUptimeShort(sys.uptime, buf, sizeof(buf));
  tft.setTextColor(COL_LABEL, COL_BG);
  tft.drawString(buf, RIGHT_X + RIGHT_W - 50, y, 1);
}

// ============================================================
//  HELPERS
// ============================================================
void formatDiff(float d, char* buf, size_t sz) {
  if (d >= 1e9)      snprintf(buf, sz, "%.1fG", d / 1e9);
  else if (d >= 1e6) snprintf(buf, sz, "%.1fM", d / 1e6);
  else if (d >= 1e3) snprintf(buf, sz, "%.1fK", d / 1e3);
  else               snprintf(buf, sz, "%.0f", d);
}

void formatUptimeShort(uint32_t s, char* buf, size_t sz) {
  uint32_t d = s / 86400;
  uint32_t h = s / 3600;
  uint32_t m = s / 60;
  if (d > 0)      snprintf(buf, sz, "%lud", (unsigned long)d);
  else if (h > 0) snprintf(buf, sz, "%luh", (unsigned long)h);
  else            snprintf(buf, sz, "%lum", (unsigned long)m);
}

void drawWrapped(const String& s, int x, int y, int maxW,
                 uint8_t font, int* outY, int lh) {
  int chunkLen = (font == 1) ? 24 : 18;
  int len = s.length();
  int pos = 0;
  while (pos < len) {
    String line = s.substring(pos, min(pos + chunkLen, len));
    tft.drawString(line, x, y, font);
    y += lh;
    pos += chunkLen;
  }
  if (outY) *outY = y;
}
