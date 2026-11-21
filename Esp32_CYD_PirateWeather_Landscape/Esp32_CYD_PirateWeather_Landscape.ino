/*
  =====================================================================================
   CYD Weather Station - Landscape Edition  v2.4
  =====================================================================================
   Hardware : ESP32-2432S028R "Cheap Yellow Display", 2.8" 320x240, landscape
   Weather  : Pirate Weather (ECMWF IFS / GFS / radar nowcast)
   Local    : Home Assistant via POST /api/template (tides, sea, outdoor station,
              indoor CO2, AQI, lightning, pressure trend, nowcast, alerts)
   Origin   : Squix / Bodmer "OpenWeather" example (MIT, see end of file)

   Pages (tap right = next, tap left third = previous):
     1 Now      animated icon, current values, 4 animated day tiles
     2 Hourly   6 tiles + 24 h temperature / rain chart
     3 8 days   tiles with high / low / rain
     4 Tide     (Home Assistant) 24 h tide curve, next high / low, waves, sea temp, moon
     5 Air      (Home Assistant) outdoor station, indoor CO2, AQI, lightning, nowcast, alerts

   Tap clock / header -> brightness menu (levels, night mode, calibrate, flip 180°)
   Long press (1.5 s) -> refresh now. Touch during boot -> touch calibration.
   Web interface on port 80: status page with live screenshot, REST API, OTA (/update).

   Libraries: TFT_eSPI (CYD User_Setup), ArduinoJson 7.x
   LittleFS: same data folder as before (icons / fonts) - no re-upload needed.
  =====================================================================================
*/

#define SERIAL_MESSAGES

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WebServer.h>
#include <Update.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include <stdarg.h>

#include "All_Settings.h"
#include "HA_Template.h"
#include "Touch.h"

#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif
#ifndef TFT_BL
#define TFT_BL 21
#endif
#ifndef ALERT_LABEL
#define ALERT_LABEL "Alert"
#endif
#ifndef TIDE_TITLE
#define TIDE_TITLE "TIDE"
#endif
#ifndef WEB_ENABLED
#define WEB_ENABLED true
#endif
#ifndef WEB_OTA
#define WEB_OTA true
#endif
#ifndef WEB_USER
#define WEB_USER "admin"
#endif
#ifndef WEB_PASS
#define WEB_PASS "change-me"
#endif

static const char* PROGRAM_VERSION = "CYD Weather Landscape v2.4";

#define AA_FONT_SMALL "fonts/NSBold15"   // 15 pt Noto Sans Bold (ASCII + °)
#define AA_FONT_LARGE "fonts/NSBold36"   // 36 pt, only 0-9 + - . : °

// ----------------------------------------------------------------------------- Colors
#define COL_SEP      0x4228
#define COL_PANEL    0x10A2
#define COL_BTN      0x2945
#define COL_BAR_OFF  0x39E7
#define COL_RAIN     0x2B39
#define COL_GRID     0x2104
#define COL_DROP     0x6D9F
#define COL_SEA      0x0190
#define COL_SEA_PAST 0x00C8
#define COL_TIDE     0x07FF

// ----------------------------------------------------------------------------- Layout
#define SCR_W        320
#define SCR_H        240
#define CONTENT_Y    46
#define NUM_PAGES    (HA_ENABLED ? 5 : 3)   // tide + air pages need Home Assistant
#define NUM_HOURS    24
#define NUM_DAYS     8
#define MAX_TIDE     8
#define MAX_ANIMS    5

// ----------------------------------------------------------------------------- Types
struct WxNow {
  uint32_t t;
  char     summary[48];
  char     icon[32];
  float    temp, feels, dew, hum, pressure, wind, gust, cloud, uv, vis, pop, precipInt;
  int      bearing;
};

struct WxHour {
  uint32_t t;
  char     icon[32];
  float    temp, pop, precipInt;
};

struct WxDay {
  uint32_t t;
  char     icon[32];
  float    tMax, tMin, pop, precipAcc, uvMax, moon;
  uint32_t sunrise, sunset;
};

struct Weather {
  bool     valid;
  uint32_t fetched;
  WxNow    now;
  WxHour   hour[NUM_HOURS];
  int      nHours;
  WxDay    day[NUM_DAYS];
  int      nDays;
  char     hourlySummary[128];
};

struct HAData {
  bool     valid;
  uint32_t fetched;
  int      nTide;
  uint32_t tideT[MAX_TIDE];
  float    tideH[MAX_TIDE];
  bool     tideHigh[MAX_TIDE];
  float    wave, sst, outT, outH, outF, aqi, pm25, co2, inT, inH, inPm, p3h;
  float    lCnt, lDist, lAz, lAge, moon, cap;
  char     tmd[48];
  char     nowcast[96];
};

struct Btn {
  int16_t x, y, w, h;
};

// Diagnostics of the last HTTP fetch (exposed via /api/status)
struct FetchDiag {
  uint32_t at;            // unix time of attempt
  int      code;          // HTTP code or negative client error
  int32_t  contentLength; // -1 = unknown
  uint32_t bytes;         // bytes consumed by the JSON parser
  uint32_t ms;            // duration
  uint32_t heapBefore;
  uint32_t maxAllocBefore;
  char     te[24];        // Transfer-Encoding
  char     ce[24];        // Content-Encoding
  char     ctype[48];
  char     calls[48];     // API quota headers
  char     head[161];
  char     tail[81];
  char     error[64];
};

// Stream wrapper: counts and records what the JSON parser actually read
class DiagStream : public Stream {
public:
  DiagStream(Stream& s, FetchDiag& d) : s_(s), d_(d) { setTimeout(20000); }
  int    available() override { return s_.available(); }
  int    read() override {
    int c = s_.read();
    if (c >= 0) rec((char)c);
    return c;
  }
  int    peek() override { return s_.peek(); }
  size_t write(uint8_t) override { return 0; }
  void   finish() {
    uint8_t n = tailLen_ < 80 ? tailLen_ : 80;
    for (uint8_t i = 0; i < n; i++) d_.tail[i] = tail_[(tailPos_ + 80 - n + i) % 80];
    d_.tail[n] = 0;
  }
private:
  void rec(char c) {
    char p = (c >= 32 && c < 127) ? c : '.';
    if (d_.bytes < 160) {
      d_.head[d_.bytes]     = p;
      d_.head[d_.bytes + 1] = 0;
    }
    tail_[tailPos_] = p;
    tailPos_ = (tailPos_ + 1) % 80;
    if (tailLen_ < 80) tailLen_++;
    d_.bytes++;
  }
  Stream&    s_;
  FetchDiag& d_;
  char       tail_[80];
  uint8_t    tailPos_ = 0;
  uint8_t    tailLen_ = 0;
};

enum FontSel { F_NONE, F_SMALL, F_LARGE };

enum AnimKind : uint8_t { A_STATIC, A_SUN, A_NIGHT, A_DRIFT, A_RAIN, A_DRIZZLE, A_SNOW, A_SLEET, A_HAIL, A_THUNDER, A_FOG, A_WIND };

struct Anim {
  int16_t      x, y;
  uint8_t      size;
  AnimKind     kind;
  uint8_t      drops;
  TFT_eSprite* base;
};

// ----------------------------------------------------------------------------- Globals
TFT_eSPI    tft = TFT_eSPI();
CYDTouch    touch;
Preferences prefs;

Weather wx;
Weather wxTmp;
HAData  ha;
HAData  haTmp;

FetchDiag diagPW;
FetchDiag diagHA;
WebServer server(80);
bool      webStarted = false;

#define LOG_LINES 40
#define LOG_LEN   110
char logBuf[LOG_LINES][LOG_LEN];
int  logHead  = 0;
int  logCount = 0;

String lastError     = "";
String haError       = "";
bool   lastFetchOk   = false;
bool   updating      = false;
bool   forceUpdate   = false;
bool   pendingRedraw = false;

uint32_t nextUpdateAt    = 0;
uint32_t nextHaAt        = 0;
uint32_t lastWifiAttempt = 0;
uint32_t boostUntil      = 0;
uint32_t lastTouchMs     = 0;
uint32_t lastFrameMs     = 0;
int      lastMinute      = -1;

int     page         = 0;
bool    overlayOpen  = false;
bool    editingNight = false;
uint8_t rotation     = SCREEN_ROTATION;
uint8_t calRotation  = SCREEN_ROTATION;

uint8_t dayLevel     = DEFAULT_DAY_LEVEL;
uint8_t nightLevel   = DEFAULT_NIGHT_LEVEL;
uint8_t currentLevel = 255;
bool    nightModeOn  = NIGHT_MODE_DEFAULT;

FontSel curFont = F_NONE;

// Animation sprites (allocated once at boot; static icons if allocation fails)
TFT_eSprite* sprBase100 = nullptr;
TFT_eSprite* sprWork100 = nullptr;
TFT_eSprite* sprBase50[4] = { nullptr, nullptr, nullptr, nullptr };
TFT_eSprite* sprWork50  = nullptr;
bool         animMemOk  = false;
Anim         anims[MAX_ANIMS];
int          nAnims     = 0;
int          nBase50    = 0;

// Perceptual 8-bit PWM curve for 12 brightness levels
const uint8_t BL_DUTY[12] = { 1, 2, 4, 7, 12, 20, 32, 50, 80, 120, 180, 255 };

// Brightness menu (landscape)
const Btn PANEL     = {   8,  48, 304, 186 };
const Btn BTN_MINUS = {  20, 122,  90,  44 };
const Btn BTN_FLIP  = { 116, 122,  88,  44 };
const Btn BTN_PLUS  = { 210, 122,  90,  44 };
const Btn BTN_NIGHT = {  20, 176,  96,  44 };
const Btn BTN_CAL   = { 124, 176,  88,  44 };
const Btn BTN_CLOSE = { 220, 176,  80,  44 };

// ----------------------------------------------------------------------------- Prototypes
void     logf(const char* fmt, ...);
void     diagBegin(FetchDiag& d);
void     diagHeaders(FetchDiag& d, HTTPClient& http);
void     webInit();
void     webStatus();
void     webIndex();
void     webLog();
void     webScreenshot();
bool     webAuth();
void     useFont(FontSel f);
bool     readBmpHeader(fs::File& f, uint32_t& offset, uint16_t& w, uint16_t& h);
void     drawBmp(const String& filename, int16_t x, int16_t y);
void     drawProgressBar(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t pct, uint16_t frame, uint16_t bar);
String   fitText(const String& s, int maxW);
void     blInit();
void     blWrite(uint8_t duty);
void     applyBacklight();
bool     loadPrefs();
void     savePrefs();
void     saveCalibration();
bool     timeValid();
bool     nightWindow();
bool     nightActive();
void     localTm(uint32_t t, struct tm& out);
String   fmtHM(uint32_t t);
bool     unitsUS();
const char* tempUnit();
const char* windUnit();
String   fmtTemp(float v);
String   fmtPct(float frac);
String   fmtRain(float acc);
String   fmtNum(float v, int dec, const char* unit);
const char* windDir8(int bearing);
uint16_t uvColor(float uv);
uint16_t aqiColor(float aqi);
uint16_t co2Color(float ppm);
String   iconFor(const char* pw, float precipInt);
int      drawWrapped(const String& text, int x, int y, int maxW, int maxLines, int lineH);
bool     fetchWeather();
bool     fetchHA();
void     printWeather();
void     printHA();
void     doUpdate();
void     doHaUpdate();
void     drawHeaderStatic();
void     drawBrightnessIcon();
void     drawPageDots();
void     drawClock();
void     drawStatus();
void     redrawPage();
void     fullRedraw();
void     drawMainPage();
void     drawHourlyPage();
void     drawHourlyChart();
void     drawDaysPage();
float    tideAt(uint32_t t, bool* rising);
void     drawTidePage();
void     drawAirPage();
void     drawHaMissing(const char* title);
bool     animInit();
AnimKind animKindFor(const String& icon, uint8_t* drops);
bool     loadIconSprite(TFT_eSprite* s, const String& path, int cropY);
void     addIcon(const String& icon, int16_t x, int16_t y, uint8_t size);
void     renderAnim(Anim& a, uint32_t t);
void     animTick(uint32_t ms);
bool     inBtn(const Btn& b, int x, int y);
void     drawButton(const Btn& b, const String& label, bool large);
void     drawOverlay();
void     drawOverlayLevel();
void     openOverlay();
void     closeOverlay();
void     overlayTap(int x, int y);
void     onTap(int x, int y);
bool     wakeIfDimmed(uint32_t ms);
void     handleTouch(uint32_t ms);
void     handleWiFi(uint32_t ms);
bool     captureRaw(int16_t& a, int16_t& b, uint32_t timeoutMs);
void     drawCross(int16_t x, int16_t y, uint16_t color);
bool     runCalibration();
void     bootMessage(const String& msg, uint8_t pct);
void     drawSplash();

// =====================================================================================
//                                       SETUP
// =====================================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println(PROGRAM_VERSION);

  // CYD RGB LED (active LOW) off
  pinMode(4, OUTPUT);  digitalWrite(4, HIGH);
  pinMode(16, OUTPUT); digitalWrite(16, HIGH);
  pinMode(17, OUTPUT); digitalWrite(17, HIGH);

  touch.begin();
  bool calStored = loadPrefs();

  tft.begin();
  tft.setRotation(rotation);
  tft.fillScreen(TFT_BLACK);

  blInit();
  applyBacklight();

  if (!LittleFS.begin()) {
    Serial.println("LittleFS init failed - data folder uploaded?");
    tft.setTextFont(2);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("LittleFS missing - upload data folder", 160, 120);
    while (true) delay(1000);
  }

  memset(&wx, 0, sizeof(wx));
  memset(&ha, 0, sizeof(ha));

  drawSplash();

  // ---------- Touch calibration: first boot, or screen touched during boot
  bootMessage(calStored ? "Touch now to recalibrate" : "Touch calibration needed", 5);
  bool wantCal = !calStored;
  uint32_t t0 = millis();
  while (!wantCal && millis() - t0 < 2500) {
    int16_t a, b, z;
    if (touch.readRaw(a, b, z)) wantCal = true;
    delay(20);
  }
  if (wantCal) {
    runCalibration();
    drawSplash();
  }

  // ---------- Animation sprites (before TLS allocates its buffers)
  animMemOk = ANIMATIONS && animInit();
  Serial.printf("Animations: %s, free heap %u\n", animMemOk ? "on" : "off", (unsigned)ESP.getFreeHeap());

  // ---------- WiFi
  bootMessage("Connecting to WiFi", 20);
  WiFi.setHostname(WIFI_HOSTNAME);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiAttempt = millis();
  t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    uint8_t pct = 20 + (uint8_t)((millis() - t0) * 40UL / 20000UL);
    drawProgressBar(10, 218, 300, 15, pct, TFT_WHITE, TFT_BLUE);
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    logf("WiFi OK, IP %s, RSSI %d", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
    bootMessage("Web: http://" + WiFi.localIP().toString(), 60);
    delay(1500);
  } else {
    Serial.println("WiFi failed - retrying in background");
    bootMessage("WiFi failed - retrying...", 60);
    delay(1500);
  }

  // ---------- Time
  bootMessage("Syncing time", 70);
  configTzTime(TZ_INFO, NTP_SERVER1, NTP_SERVER2);
  t0 = millis();
  while (!timeValid() && millis() - t0 < 10000) delay(200);

  if (WEB_ENABLED) webInit();

  bootMessage("Loading weather", 90);
  delay(300);

  tft.fillScreen(TFT_BLACK);
  drawHeaderStatic();
  drawClock();
  nextUpdateAt = millis();
  nextHaAt     = millis() + 500;
  lastTouchMs  = millis();
}

// =====================================================================================
//                                       LOOP
// =====================================================================================
void loop() {
  uint32_t ms = millis();

  handleWiFi(ms);
  if (webStarted) server.handleClient();
  handleTouch(ms);

  if (forceUpdate || (int32_t)(ms - nextUpdateAt) >= 0) {
    bool both = forceUpdate;
    forceUpdate = false;
    doUpdate();
    if (both) doHaUpdate();
    ms = millis();
  }

  if (HA_ENABLED && (int32_t)(ms - nextHaAt) >= 0) {
    doHaUpdate();
    ms = millis();
  }

  if (timeValid()) {
    struct tm tmNow;
    localTm((uint32_t)time(nullptr), tmNow);
    if (tmNow.tm_min != lastMinute) {
      lastMinute = tmNow.tm_min;
      drawClock();
      if (page == 3 && !overlayOpen && tmNow.tm_min % 10 == 0) redrawPage();   // move tide "now" marker
    }
  }

  // signed difference: web handlers set lastTouchMs after 'ms' was taken
  int32_t idleMs = (int32_t)(millis() - lastTouchMs);
  if (overlayOpen && idleMs > (int32_t)(OVERLAY_TIMEOUT_S * 1000UL)) closeOverlay();

  if (!overlayOpen && PAGE_TIMEOUT_S > 0 && page != 0 && idleMs > (int32_t)(PAGE_TIMEOUT_S * 1000UL)) {
    page = 0;
    redrawPage();
  }

  animTick(ms);
  applyBacklight();
  delay(5);
}

// =====================================================================================
//                                  Fonts / graphics
// =====================================================================================
void useFont(FontSel f) {
  if (f == curFont) return;
  if (curFont != F_NONE) tft.unloadFont();
  if (f == F_SMALL) {
    tft.loadFont(AA_FONT_SMALL, LittleFS);
  } else if (f == F_LARGE) {
    tft.loadFont(AA_FONT_LARGE, LittleFS);
  } else {
    tft.setTextFont(1);
    tft.setTextSize(1);
  }
  curFont = f;
}

static uint16_t rd16(fs::File& f) {
  uint16_t r;
  ((uint8_t*)&r)[0] = f.read();
  ((uint8_t*)&r)[1] = f.read();
  return r;
}

static uint32_t rd32(fs::File& f) {
  uint32_t r;
  ((uint8_t*)&r)[0] = f.read();
  ((uint8_t*)&r)[1] = f.read();
  ((uint8_t*)&r)[2] = f.read();
  ((uint8_t*)&r)[3] = f.read();
  return r;
}

// 24-bit uncompressed BMP header -> pixel offset, width, height
bool readBmpHeader(fs::File& f, uint32_t& offset, uint16_t& w, uint16_t& h) {
  if (rd16(f) != 0x4D42) return false;
  rd32(f);
  rd32(f);
  offset = rd32(f);
  rd32(f);
  w = (uint16_t)rd32(f);
  h = (uint16_t)rd32(f);
  if (rd16(f) != 1) return false;
  if (rd16(f) != 24) return false;
  if (rd32(f) != 0) return false;
  return true;
}

// Bodmer's "no seek" BMP renderer
void drawBmp(const String& filename, int16_t x, int16_t y) {
  if (x >= tft.width() || y >= tft.height()) return;
  if (!LittleFS.exists(filename)) {
    Serial.print("BMP missing: ");
    Serial.println(filename);
    return;
  }
  fs::File f = LittleFS.open(filename, "r");
  if (!f) return;
  uint32_t offset;
  uint16_t w, h;
  if (readBmpHeader(f, offset, w, h)) {
    int16_t yy = y + h - 1;
    bool oldSwap = tft.getSwapBytes();
    tft.setSwapBytes(true);
    f.seek(offset);
    uint16_t padding = (4 - ((w * 3) & 3)) & 3;
    uint8_t lineBuffer[w * 3 + padding];
    for (uint16_t row = 0; row < h; row++) {
      f.read(lineBuffer, sizeof(lineBuffer));
      uint8_t*  bptr = lineBuffer;
      uint16_t* tptr = (uint16_t*)lineBuffer;
      for (uint16_t col = 0; col < w; col++) {
        uint8_t b = *bptr++;
        uint8_t g = *bptr++;
        uint8_t r = *bptr++;
        *tptr++ = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
      }
      tft.pushImage(x, yy--, w, 1, (uint16_t*)lineBuffer);
    }
    tft.setSwapBytes(oldSwap);
  } else {
    Serial.println("BMP format not supported");
  }
  f.close();
}

void drawProgressBar(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t pct, uint16_t frame, uint16_t bar) {
  if (pct > 100) pct = 100;
  tft.drawRoundRect(x, y, w, h, 3, frame);
  tft.fillRect(x + 2, y + 2, (w - 4) * pct / 100, h - 4, bar);
}

String fitText(const String& s, int maxW) {
  if (tft.textWidth(s) <= maxW) return s;
  String r = s;
  while (r.length() > 0 && tft.textWidth(r + "..") > maxW) r.remove(r.length() - 1);
  r.trim();
  return r + "..";
}

// =====================================================================================
//                                     Backlight
// =====================================================================================
#define BL_LEDC_CHANNEL 7

void blInit() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(TFT_BL, 5000, 8);
#else
  ledcSetup(BL_LEDC_CHANNEL, 5000, 8);
  ledcAttachPin(TFT_BL, BL_LEDC_CHANNEL);
#endif
  currentLevel = 255;
}

void blWrite(uint8_t duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(TFT_BL, duty);
#else
  ledcWrite(BL_LEDC_CHANNEL, duty);
#endif
}

void applyBacklight() {
  uint8_t lvl;
  if (overlayOpen) lvl = editingNight ? nightLevel : dayLevel;
  else if (nightActive() && (int32_t)(millis() - boostUntil) >= 0) lvl = nightLevel;
  else lvl = dayLevel;
  if (lvl > 11) lvl = 11;
  if (lvl != currentLevel) {
    blWrite(BL_DUTY[lvl]);
    currentLevel = lvl;
  }
}

// =====================================================================================
//                                   Settings (NVS)
// =====================================================================================
bool loadPrefs() {
  prefs.begin("cydwx2", false);
  dayLevel    = prefs.getUChar("day", DEFAULT_DAY_LEVEL);
  nightLevel  = prefs.getUChar("night", DEFAULT_NIGHT_LEVEL);
  nightModeOn = prefs.getBool("nightOn", NIGHT_MODE_DEFAULT);
  rotation    = prefs.getUChar("rot", SCREEN_ROTATION);
  bool cal    = prefs.getBool("calOK", false);
  if (cal) {
    calRotation  = prefs.getUChar("cRot", rotation);
    touch.swapXY = prefs.getBool("cSwap", false);
    touch.xRaw0  = prefs.getShort("cx0", touch.xRaw0);
    touch.xRaw1  = prefs.getShort("cx1", touch.xRaw1);
    touch.yRaw0  = prefs.getShort("cy0", touch.yRaw0);
    touch.yRaw1  = prefs.getShort("cy1", touch.yRaw1);
  }
  prefs.end();
  if (rotation != 1 && rotation != 3) rotation = 1;
  if (dayLevel > 11) dayLevel = 11;
  if (nightLevel > 11) nightLevel = 11;
  if (cal && calRotation != 1 && calRotation != 3) cal = false;
  calRotation = cal ? calRotation : rotation;
  touch.flip  = (rotation != calRotation);
  Serial.printf("Prefs: day=%u night=%u nightMode=%d rot=%u cal=%d flip=%d\n",
                dayLevel, nightLevel, nightModeOn, rotation, cal, touch.flip);
  return cal;
}

void savePrefs() {
  prefs.begin("cydwx2", false);
  prefs.putUChar("day", dayLevel);
  prefs.putUChar("night", nightLevel);
  prefs.putBool("nightOn", nightModeOn);
  prefs.putUChar("rot", rotation);
  prefs.end();
}

void saveCalibration() {
  prefs.begin("cydwx2", false);
  prefs.putUChar("cRot", calRotation);
  prefs.putBool("cSwap", touch.swapXY);
  prefs.putShort("cx0", touch.xRaw0);
  prefs.putShort("cx1", touch.xRaw1);
  prefs.putShort("cy0", touch.yRaw0);
  prefs.putShort("cy1", touch.yRaw1);
  prefs.putBool("calOK", true);
  prefs.end();
}

// =====================================================================================
//                                   Time helpers
// =====================================================================================
bool timeValid() {
  return time(nullptr) > 1700000000;
}

void localTm(uint32_t t, struct tm& out) {
  time_t tt = (time_t)t;
  localtime_r(&tt, &out);
}

bool nightWindow() {
  if (!timeValid()) return false;
  struct tm tmv;
  localTm((uint32_t)time(nullptr), tmv);
  int m     = tmv.tm_hour * 60 + tmv.tm_min;
  int start = NIGHT_START_HOUR * 60 + NIGHT_START_MINUTE;
  int end   = NIGHT_END_HOUR * 60 + NIGHT_END_MINUTE;
  if (start == end) return false;
  if (start < end) return m >= start && m < end;
  return m >= start || m < end;
}

bool nightActive() {
  return nightModeOn && nightWindow();
}

String fmtHM(uint32_t t) {
  if (t == 0) return "--:--";
  struct tm tmv;
  localTm(t, tmv);
  char b[8];
  snprintf(b, sizeof(b), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
  return String(b);
}

// =====================================================================================
//                                  Units / format
// =====================================================================================
bool unitsUS() { return strcmp(PW_UNITS, "us") == 0; }

const char* tempUnit() { return unitsUS() ? "°F" : "°C"; }

const char* windUnit() {
  if (strcmp(PW_UNITS, "ca") == 0) return "km/h";
  if (strcmp(PW_UNITS, "si") == 0) return "m/s";
  return "mph";
}

String fmtTemp(float v) {
  if (isnan(v)) return "--";
  return String((int)lroundf(v)) + "°";
}

String fmtPct(float frac) {
  if (isnan(frac)) return "--";
  return String((int)lroundf(frac * 100.0f)) + "%";
}

// Pirate Weather precipAccumulation: cm (si/ca/uk) or inch (us)
String fmtRain(float acc) {
  if (isnan(acc)) return "--";
  if (unitsUS()) return String(acc, 2) + "in";
  return String(acc * 10.0f, 1) + "mm";
}

String fmtNum(float v, int dec, const char* unit) {
  if (isnan(v)) return "--";
  return String(v, dec) + unit;
}

const char* windDir8(int bearing) {
  static const char* d[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
  int b = ((bearing % 360) + 360) % 360;
  return d[((int)((b + 22.5f) / 45.0f)) % 8];
}

uint16_t uvColor(float uv) {
  if (isnan(uv) || uv < 3) return TFT_GREEN;
  if (uv < 6) return TFT_YELLOW;
  if (uv < 8) return TFT_ORANGE;
  if (uv < 11) return TFT_RED;
  return TFT_VIOLET;
}

uint16_t aqiColor(float aqi) {
  if (isnan(aqi)) return TFT_LIGHTGREY;
  if (aqi <= 50) return TFT_GREEN;
  if (aqi <= 100) return TFT_YELLOW;
  if (aqi <= 150) return TFT_ORANGE;
  return TFT_RED;
}

// PM2.5 in ug/m3 (WHO / US EPA style bands)
uint16_t pmColor(float pm) {
  if (isnan(pm)) return TFT_LIGHTGREY;
  if (pm <= 15) return TFT_GREEN;
  if (pm <= 35) return TFT_YELLOW;
  if (pm <= 55) return TFT_ORANGE;
  return TFT_RED;
}

uint16_t co2Color(float ppm) {
  if (isnan(ppm)) return TFT_LIGHTGREY;
  if (ppm < 800) return TFT_GREEN;
  if (ppm < 1200) return TFT_YELLOW;
  return TFT_RED;
}

// Pirate icon -> file name in LittleFS (/icon/*.bmp, /icon50/*.bmp)
String iconFor(const char* pw, float precipInt) {
  String s(pw);
  s.toLowerCase();
  if (s.indexOf("thunder") >= 0) return "thunderstorm";
  if (s.indexOf("hail") >= 0) return "hail";
  if (s.indexOf("sleet") >= 0 || s.indexOf("mixed") >= 0 || s.indexOf("freezing") >= 0) return "sleet";
  if (s.indexOf("snow") >= 0 || s.indexOf("flurr") >= 0) return "snow";
  if (s.indexOf("drizzle") >= 0) return "drizzle";
  if (s.indexOf("light-rain") >= 0 || s.indexOf("possible") >= 0) return "lightRain";
  if (s.indexOf("rain") >= 0 || s.indexOf("precip") >= 0) {
    if (!isnan(precipInt)) {
      float drizzleMax = unitsUS() ? 0.01f : 0.25f;
      float lightMax   = unitsUS() ? 0.04f : 1.0f;
      if (precipInt < drizzleMax) return "drizzle";
      if (precipInt < lightMax) return "lightRain";
    }
    return "rain";
  }
  if (s.indexOf("fog") >= 0 || s.indexOf("mist") >= 0 || s.indexOf("haze") >= 0 || s.indexOf("smoke") >= 0) return "fog";
  if (s.indexOf("wind") >= 0 || s.indexOf("breezy") >= 0) return "wind";
  bool partly = s.indexOf("partly") >= 0 || s.indexOf("mostly-clear") >= 0;
  if (partly && s.indexOf("night") >= 0) return "partly-cloudy-night";
  if (partly) return "partly-cloudy-day";
  if (s.indexOf("cloudy") >= 0 || s.indexOf("overcast") >= 0) return "cloudy";
  if (s.indexOf("clear") >= 0 && s.indexOf("night") >= 0) return "clear-night";
  if (s.indexOf("clear") >= 0) return "clear-day";
  return "unknown";
}

// Word wrap, last line shortened with "...". Datum TL.
int drawWrapped(const String& text, int x, int y, int maxW, int maxLines, int lineH) {
  String rest = text;
  rest.trim();
  int line = 0;
  tft.setTextDatum(TL_DATUM);
  while (rest.length() > 0 && line < maxLines) {
    int cut = rest.length();
    if (tft.textWidth(rest) > maxW) {
      int lastSpace = -1;
      for (int i = 0; i < (int)rest.length(); i++) {
        if (rest[i] == ' ') {
          if (tft.textWidth(rest.substring(0, i)) <= maxW) lastSpace = i;
          else break;
        }
      }
      if (lastSpace > 0) cut = lastSpace;
    }
    String ln = rest.substring(0, cut);
    if (line == maxLines - 1 && cut < (int)rest.length()) {
      while (ln.length() > 0 && tft.textWidth(ln + "...") > maxW) ln.remove(ln.length() - 1);
      ln += "...";
    }
    tft.drawString(ln, x, y + line * lineH);
    rest = rest.substring(cut);
    rest.trim();
    line++;
  }
  return line;
}

// =====================================================================================
//                                   Pirate Weather
// =====================================================================================
bool fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) {
    lastError = "no WiFi";
    return false;
  }

  String url = String("https://api.pirateweather.net/forecast/") + PW_API_KEY + "/" +
               LATITUDE + "," + LONGITUDE +
               "?units=" + PW_UNITS + "&lang=" + PW_LANG +
               "&exclude=minutely,alerts,flags";

  diagBegin(diagPW);
  uint32_t t0 = millis();

  WiFiClientSecure client;
  client.setInsecure();               // weather data - certificate not checked

  HTTPClient http;
  http.useHTTP10(true);               // no chunked encoding -> stream directly
  http.setConnectTimeout(10000);
  http.setTimeout(20000);
  const char* hdrKeys[] = { "Content-Length", "Transfer-Encoding", "Content-Encoding", "Content-Type",
                            "ratelimit-remaining", "x-forecast-api-calls" };
  http.collectHeaders(hdrKeys, 6);

  if (!http.begin(client, url)) {
    lastError = "http.begin";
    strlcpy(diagPW.error, lastError.c_str(), sizeof(diagPW.error));
    return false;
  }
  http.addHeader("Accept-Encoding", "identity");

  int code = http.GET();
  diagPW.code = code;
  if (code != HTTP_CODE_OK) {
    lastError = (code < 0) ? String("conn ") + http.errorToString(code) : String("HTTP ") + code;
    if (code == 401 || code == 403) lastError += " key?";
    if (code == 429) lastError += " quota";
    strlcpy(diagPW.error, lastError.c_str(), sizeof(diagPW.error));
    diagPW.ms = millis() - t0;
    logf("PW fetch failed: %s", lastError.c_str());
    http.end();
    return false;
  }
  diagHeaders(diagPW, http);

  JsonDocument filter;
  const char* curF[] = { "time", "summary", "icon", "temperature", "apparentTemperature", "dewPoint",
                         "humidity", "pressure", "windSpeed", "windGust", "windBearing", "cloudCover",
                         "uvIndex", "visibility", "precipProbability", "precipIntensity" };
  for (const char* k : curF) filter["currently"][k] = true;
  filter["hourly"]["summary"] = true;
  const char* hF[] = { "time", "icon", "temperature", "precipProbability", "precipIntensity" };
  for (const char* k : hF) filter["hourly"]["data"][0][k] = true;
  const char* dF[] = { "time", "icon", "temperatureMax", "temperatureMin", "precipProbability",
                       "precipAccumulation", "uvIndex", "moonPhase", "sunriseTime", "sunsetTime" };
  for (const char* k : dF) filter["daily"]["data"][0][k] = true;

  JsonDocument doc;
  DiagStream ds(http.getStream(), diagPW);
  DeserializationError err = deserializeJson(doc, ds, DeserializationOption::Filter(filter));
  ds.finish();
  http.end();
  diagPW.ms = millis() - t0;
  if (err) {
    lastError = String("JSON ") + err.c_str();
    strlcpy(diagPW.error, lastError.c_str(), sizeof(diagPW.error));
    logf("PW JSON %s after %u of %d bytes, %u ms, TE '%s' CE '%s'", err.c_str(), (unsigned)diagPW.bytes,
         (int)diagPW.contentLength, (unsigned)diagPW.ms, diagPW.te, diagPW.ce);
    return false;
  }
  logf("PW OK: %u bytes, %u ms, heap %u", (unsigned)diagPW.bytes, (unsigned)diagPW.ms, (unsigned)ESP.getFreeHeap());

  memset(&wxTmp, 0, sizeof(wxTmp));
  JsonObject c = doc["currently"];
  if (c.isNull()) {
    lastError = "no data";
    return false;
  }
  wxTmp.now.t = c["time"] | 0UL;
  strlcpy(wxTmp.now.summary, c["summary"] | "", sizeof(wxTmp.now.summary));
  strlcpy(wxTmp.now.icon, c["icon"] | "", sizeof(wxTmp.now.icon));
  wxTmp.now.temp      = c["temperature"] | NAN;
  wxTmp.now.feels     = c["apparentTemperature"] | NAN;
  wxTmp.now.dew       = c["dewPoint"] | NAN;
  wxTmp.now.hum       = c["humidity"] | NAN;
  wxTmp.now.pressure  = c["pressure"] | NAN;
  wxTmp.now.wind      = c["windSpeed"] | NAN;
  wxTmp.now.gust      = c["windGust"] | NAN;
  wxTmp.now.bearing   = c["windBearing"] | 0;
  wxTmp.now.cloud     = c["cloudCover"] | NAN;
  wxTmp.now.uv        = c["uvIndex"] | NAN;
  wxTmp.now.vis       = c["visibility"] | NAN;
  wxTmp.now.pop       = c["precipProbability"] | NAN;
  wxTmp.now.precipInt = c["precipIntensity"] | NAN;
  strlcpy(wxTmp.hourlySummary, doc["hourly"]["summary"] | "", sizeof(wxTmp.hourlySummary));

  int n = 0;
  for (JsonObject h : doc["hourly"]["data"].as<JsonArray>()) {
    if (n >= NUM_HOURS) break;
    WxHour& wh = wxTmp.hour[n];
    wh.t = h["time"] | 0UL;
    strlcpy(wh.icon, h["icon"] | "", sizeof(wh.icon));
    wh.temp      = h["temperature"] | NAN;
    wh.pop       = h["precipProbability"] | NAN;
    wh.precipInt = h["precipIntensity"] | NAN;
    n++;
  }
  wxTmp.nHours = n;

  n = 0;
  for (JsonObject d : doc["daily"]["data"].as<JsonArray>()) {
    if (n >= NUM_DAYS) break;
    WxDay& wd = wxTmp.day[n];
    wd.t = d["time"] | 0UL;
    strlcpy(wd.icon, d["icon"] | "", sizeof(wd.icon));
    wd.tMax      = d["temperatureMax"] | NAN;
    wd.tMin      = d["temperatureMin"] | NAN;
    wd.pop       = d["precipProbability"] | NAN;
    wd.precipAcc = d["precipAccumulation"] | NAN;
    wd.uvMax     = d["uvIndex"] | NAN;
    wd.moon      = d["moonPhase"] | NAN;
    wd.sunrise   = d["sunriseTime"] | 0UL;
    wd.sunset    = d["sunsetTime"] | 0UL;
    n++;
  }
  wxTmp.nDays = n;

  if (wxTmp.nHours < 1 || wxTmp.nDays < 1) {
    lastError = "incomplete";
    return false;
  }
  wxTmp.valid   = true;
  wxTmp.fetched = timeValid() ? (uint32_t)time(nullptr) : wxTmp.now.t;
  wx = wxTmp;
  lastError = "";
  return true;
}

// =====================================================================================
//                                   Home Assistant
// =====================================================================================
bool fetchHA() {
  if (WiFi.status() != WL_CONNECTED) {
    haError = "no WiFi";
    return false;
  }

  WiFiClient       plain;
  WiFiClientSecure secure;
  secure.setInsecure();
  bool tls = strncmp(HA_URL, "https", 5) == 0;

  diagBegin(diagHA);
  uint32_t t0 = millis();

  HTTPClient http;
  http.useHTTP10(true);
  http.setConnectTimeout(5000);
  http.setTimeout(8000);
  const char* hdrKeys[] = { "Content-Length", "Transfer-Encoding", "Content-Encoding", "Content-Type" };
  http.collectHeaders(hdrKeys, 4);
  String url = String(HA_URL) + "/api/template";
  bool ok = tls ? http.begin(secure, url) : http.begin(plain, url);
  if (!ok) {
    haError = "http.begin";
    strlcpy(diagHA.error, haError.c_str(), sizeof(diagHA.error));
    return false;
  }
  http.addHeader("Authorization", String("Bearer ") + HA_TOKEN);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept-Encoding", "identity");

  String body;
  {
    JsonDocument req;
    req["template"] = HA_TEMPLATE;
    serializeJson(req, body);
  }

  int code = http.POST(body);
  diagHA.code = code;
  if (code != HTTP_CODE_OK) {
    haError = (code < 0) ? String("conn ") + http.errorToString(code) : String("HTTP ") + code;
    if (code == 401) haError += " token?";
    if (code == 400) haError += " template?";
    strlcpy(diagHA.error, haError.c_str(), sizeof(diagHA.error));
    diagHA.ms = millis() - t0;
    logf("HA fetch failed: %s", haError.c_str());
    http.end();
    return false;
  }
  diagHeaders(diagHA, http);

  JsonDocument doc;
  DiagStream ds(http.getStream(), diagHA);
  DeserializationError err = deserializeJson(doc, ds);
  ds.finish();
  http.end();
  diagHA.ms = millis() - t0;
  if (err) {
    haError = String("JSON ") + err.c_str();
    strlcpy(diagHA.error, haError.c_str(), sizeof(diagHA.error));
    logf("HA JSON %s after %u bytes", err.c_str(), (unsigned)diagHA.bytes);
    return false;
  }
  logf("HA OK: %u bytes, %u ms", (unsigned)diagHA.bytes, (unsigned)diagHA.ms);

  memset(&haTmp, 0, sizeof(haTmp));
  int n = 0;
  for (JsonArray e : doc["tide"].as<JsonArray>()) {
    if (n >= MAX_TIDE) break;
    haTmp.tideT[n]    = e[0] | 0UL;
    haTmp.tideH[n]    = e[1] | NAN;
    haTmp.tideHigh[n] = (e[2] | 0) == 1;
    n++;
  }
  haTmp.nTide = n;
  haTmp.wave  = doc["wave"] | NAN;
  haTmp.sst   = doc["sst"] | NAN;
  haTmp.outT  = doc["out_t"] | NAN;
  haTmp.outH  = doc["out_h"] | NAN;
  haTmp.outF  = doc["out_f"] | NAN;
  haTmp.aqi   = doc["aqi"] | NAN;
  haTmp.pm25  = doc["pm25"] | NAN;
  haTmp.co2   = doc["co2"] | NAN;
  haTmp.inT   = doc["in_t"] | NAN;
  haTmp.inH   = doc["in_h"] | NAN;
  haTmp.inPm  = doc["in_pm"] | NAN;
  haTmp.p3h   = doc["p3h"] | NAN;
  haTmp.lCnt  = doc["l_cnt"] | NAN;
  haTmp.lDist = doc["l_dist"] | NAN;
  haTmp.lAz   = doc["l_az"] | NAN;
  haTmp.lAge  = doc["l_age"] | NAN;
  haTmp.moon  = doc["moon"] | NAN;
  haTmp.cap   = doc["cap"] | NAN;
  strlcpy(haTmp.tmd, doc["tmd"] | "", sizeof(haTmp.tmd));
  strlcpy(haTmp.nowcast, doc["nowcast"] | "", sizeof(haTmp.nowcast));
  haTmp.valid   = true;
  haTmp.fetched = timeValid() ? (uint32_t)time(nullptr) : (uint32_t)(doc["t"] | 0UL);
  ha = haTmp;
  haError = "";
  return true;
}

void printWeather() {
#ifdef SERIAL_MESSAGES
  Serial.println("------------- Pirate Weather -------------");
  Serial.printf("Now     : %s (%s -> %s)\n", wx.now.summary, wx.now.icon, iconFor(wx.now.icon, wx.now.precipInt).c_str());
  Serial.printf("Temp    : %.1f  feels %.1f  dew %.1f  hum %.0f%%\n", wx.now.temp, wx.now.feels, wx.now.dew, wx.now.hum * 100);
  Serial.printf("Wind    : %.1f %s gust %.1f  %d deg  pressure %.0f\n", wx.now.wind, windUnit(), wx.now.gust, wx.now.bearing, wx.now.pressure);
  Serial.printf("Hourly  : %s (%d h)\n", wx.hourlySummary, wx.nHours);
  for (int i = 0; i < wx.nDays; i++) {
    struct tm tmv;
    localTm(wx.day[i].t, tmv);
    Serial.printf("  %s  %5.1f / %5.1f  pop %3.0f%%  %s\n", DOW_SHORT[tmv.tm_wday],
                  wx.day[i].tMax, wx.day[i].tMin, wx.day[i].pop * 100, wx.day[i].icon);
  }
  Serial.printf("Heap    : %u\n", (unsigned)ESP.getFreeHeap());
#endif
}

void printHA() {
#ifdef SERIAL_MESSAGES
  Serial.println("------------- Home Assistant -------------");
  for (int i = 0; i < ha.nTide; i++)
    Serial.printf("  tide %s %s %.2f m\n", ha.tideHigh[i] ? "HIGH" : "low ", fmtHM(ha.tideT[i]).c_str(), ha.tideH[i]);
  Serial.printf("Sea     : wave %.2f m  sst %.1f\n", ha.wave, ha.sst);
  Serial.printf("Outdoor : %.1f  %.0f%%  feels %.1f  AQI %.0f  PM2.5 %.0f\n", ha.outT, ha.outH, ha.outF, ha.aqi, ha.pm25);
  Serial.printf("Indoor  : CO2 %.0f  %.1f  %.0f%%   p3h %.1f\n", ha.co2, ha.inT, ha.inH, ha.p3h);
  Serial.printf("Lightn. : cnt %.0f dist %.0f az %.0f age %.0f min | moon %.0f%% | CAP %.0f | TMD %s\n",
                ha.lCnt, ha.lDist, ha.lAz, ha.lAge, ha.moon, ha.cap, ha.tmd);
  Serial.printf("Nowcast : %s\n", ha.nowcast);
#endif
}

void doUpdate() {
  updating = true;
  drawStatus();
  bool ok = fetchWeather();
  updating     = false;
  lastFetchOk  = ok;
  nextUpdateAt = millis() + (ok ? UPDATE_INTERVAL_MIN * 60000UL : RETRY_INTERVAL_S * 1000UL);
  if (ok) printWeather();
  else Serial.println("Weather update failed: " + lastError);
  drawStatus();
  if (ok || !wx.valid) {
    if (overlayOpen) pendingRedraw = true;
    else redrawPage();
  }
}

void doHaUpdate() {
  if (!HA_ENABLED) return;
  bool ok  = fetchHA();
  nextHaAt = millis() + (ok ? HA_INTERVAL_S * 1000UL : RETRY_INTERVAL_S * 1000UL);
  if (ok) printHA();
  else Serial.println("HA update failed: " + haError);
  // page 0 is not redrawn (would restart the animations), it picks up HA data on next draw
  if (page == 3 || page == 4) {
    if (overlayOpen) pendingRedraw = true;
    else redrawPage();
  }
}

// =====================================================================================
//                                      Header
// =====================================================================================
void drawHeaderStatic() {
  drawBrightnessIcon();
  drawPageDots();
  tft.drawFastHLine(6, 45, 308, COL_SEP);
  drawStatus();
}

// Small sun = hint "tap here for brightness"
void drawBrightnessIcon() {
  const int cx = 13, cy = 22;
  tft.fillRect(1, 10, 25, 25, TFT_BLACK);
  uint16_t col = nightActive() ? TFT_SKYBLUE : TFT_YELLOW;
  tft.fillCircle(cx, cy, 4, col);
  for (int i = 0; i < 8; i++) {
    float a = i * 45.0f * DEG_TO_RAD;
    tft.drawLine(cx + cosf(a) * 7, cy + sinf(a) * 7, cx + cosf(a) * 10, cy + sinf(a) * 10, col);
  }
}

void drawPageDots() {
  for (int i = 0; i < NUM_PAGES; i++) {
    int x = 152 + i * 10;
    tft.fillCircle(x, 36, 3, TFT_BLACK);
    if (i == page) tft.fillCircle(x, 36, 3, TFT_WHITE);
    else tft.drawCircle(x, 36, 3, TFT_DARKGREY);
  }
}

void drawClock() {
  String timeNow = "--:--";
  String dateNow = "";
  if (timeValid()) {
    struct tm tmv;
    localTm((uint32_t)time(nullptr), tmv);
    char b[24];
    snprintf(b, sizeof(b), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    timeNow = b;
    snprintf(b, sizeof(b), "%s %d %s", DOW_SHORT[tmv.tm_wday], tmv.tm_mday, MONTH_SHORT[tmv.tm_mon]);
    dateNow = b;
  }
  useFont(F_LARGE);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextPadding(tft.textWidth("44:44 "));
  tft.drawString(timeNow, 30, 4);
  tft.setTextPadding(0);

  useFont(F_SMALL);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextPadding(150);
  tft.drawString(dateNow, 314, 4);
  tft.setTextPadding(0);

  tft.drawFastHLine(6, 45, 308, COL_SEP);
  drawBrightnessIcon();
}

void drawStatus() {
  String s;
  uint16_t col = TFT_ORANGE;
  if (updating) {
    s = "Updating...";
    col = TFT_CYAN;
  } else if (!wx.valid) {
    s = lastError.length() ? "Err: " + lastError : "No data yet";
    col = TFT_RED;
  } else if (!lastFetchOk) {
    s = "Err: " + lastError;
    col = TFT_RED;
  } else {
    s = "Updated " + fmtHM(wx.fetched);
  }
  useFont(F_SMALL);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(col, TFT_BLACK);
  tft.setTextPadding(118);                 // stays right of the page dots (x <= 195)
  tft.drawString(fitText(s, 118), 314, 22);
  tft.setTextPadding(0);
}

// =====================================================================================
//                                       Pages
// =====================================================================================
void redrawPage() {
  nAnims  = 0;
  nBase50 = 0;
  tft.fillRect(0, CONTENT_Y, SCR_W, SCR_H - CONTENT_Y, TFT_BLACK);
  drawPageDots();
  pendingRedraw = false;

  if (page <= 2 && !wx.valid) {
    useFont(F_SMALL);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(updating ? "Loading weather..." : "No weather data", 160, 110);
    if (lastError.length()) {
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.drawString(lastError, 160, 135);
    }
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("Long press = retry", 160, 175);
    return;
  }
  switch (page) {
    case 0:  drawMainPage();   break;
    case 1:  drawHourlyPage(); break;
    case 2:  drawDaysPage();   break;
    case 3:  drawTidePage();   break;
    default: drawAirPage();    break;
  }
}

void fullRedraw() {
  tft.fillScreen(TFT_BLACK);
  drawHeaderStatic();
  drawClock();
  redrawPage();
}

// ------------------------------------------------------------------ Page 1: Now
void drawMainPage() {
  const WxNow& n = wx.now;
  const WxDay& t = wx.day[0];

  addIcon(iconFor(n.icon, n.precipInt), 2, 48, 100);

  useFont(F_SMALL);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextDatum(TR_DATUM);
  tft.drawString("Feels " + fmtTemp(n.feels), 316, 54);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Hum " + fmtPct(n.hum), 316, 73);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.drawString(fitText(n.summary, 208), 108, 94);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(String("Wind ") + (int)lroundf(n.wind) + " " + windUnit() + " " + windDir8(n.bearing), 108, 113);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(uvColor(t.uvMax), TFT_BLACK);
  tft.drawString("UV " + (isnan(t.uvMax) ? String("--") : String((int)lroundf(t.uvMax))), 316, 113);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_SKYBLUE, TFT_BLACK);
  tft.drawString("Rain today " + fmtPct(t.pop), 108, 132);
  tft.setTextDatum(TR_DATUM);
  if (ha.valid && !isnan(ha.sst)) {
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString("Sea " + fmtTemp(ha.sst), 316, 132);
  } else {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Dew " + fmtTemp(n.dew), 316, 132);
  }

  tft.drawFastHLine(6, 155, 308, COL_SEP);

  // 4 day tiles (tomorrow .. +4), animated icons
  for (int x = 80; x < SCR_W; x += 80) tft.drawFastVLine(x, 160, 74, COL_GRID);
  for (int k = 0; k < 4; k++) {
    int idx = k + 1;
    if (idx >= wx.nDays) break;
    const WxDay& d = wx.day[idx];
    int x = k * 80;
    struct tm tmv;
    localTm(d.t, tmv);
    useFont(F_SMALL);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawString(DOW_SHORT[tmv.tm_wday], x + 5, 160);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(TFT_SKYBLUE, TFT_BLACK);
    tft.drawString(fmtPct(d.pop), x + 77, 160);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(fmtTemp(d.tMax), x + 77, 186);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(fmtTemp(d.tMin), x + 77, 206);
    addIcon(iconFor(d.icon, NAN), x + 1, 176, 50);
  }

  // Big temperature last (single font switch)
  useFont(F_LARGE);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  String temp = isnan(n.temp) ? String("--") : String((int)lroundf(n.temp));
  tft.drawString(temp, 108, 52);
  int tw = tft.textWidth(temp);
  useFont(F_SMALL);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString(tempUnit(), 110 + tw, 54);
}

// ------------------------------------------------------------------ Page 2: Hourly
void drawHourlyPage() {
  useFont(F_SMALL);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(fitText(wx.hourlySummary, 308), 6, 49);

  for (int k = 0; k < 6; k++) {
    int i = 1 + k * HOURLY_STEP;
    if (i >= wx.nHours) break;
    const WxHour& h = wx.hour[i];
    int cx = 27 + k * 53;
    tft.setTextDatum(BC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(fmtHM(h.t), cx, 82);
    drawBmp("/icon50/" + iconFor(h.icon, h.precipInt) + ".bmp", cx - 25, 84);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString(fmtTemp(h.temp), cx, 152);
    tft.setTextColor(TFT_SKYBLUE, TFT_BLACK);
    tft.drawString(fmtPct(h.pop), cx, 169);
  }
  drawHourlyChart();
}

void drawHourlyChart() {
  const int X0 = 24, X1 = 316, Y0 = 186, Y1 = 226;
  int n = wx.nHours < NUM_HOURS ? wx.nHours : NUM_HOURS;
  if (n < 2) return;

  float tmin = 1e9f, tmax = -1e9f;
  for (int i = 0; i < n; i++) {
    float v = wx.hour[i].temp;
    if (isnan(v)) continue;
    if (v < tmin) tmin = v;
    if (v > tmax) tmax = v;
  }
  if (tmin > tmax) return;
  tmin = floorf(tmin) - 1.0f;
  tmax = ceilf(tmax) + 1.0f;

  float bw = (float)(X1 - X0) / n;
  auto px = [&](int i) -> float { return X0 + i * bw + bw / 2.0f; };
  auto py = [&](float t) -> float { return Y1 - (t - tmin) / (tmax - tmin) * (Y1 - Y0); };

  tft.drawFastHLine(X0, Y0, X1 - X0, COL_GRID);
  tft.drawFastHLine(X0, (Y0 + Y1) / 2, X1 - X0, COL_GRID);
  tft.drawFastHLine(X0, Y1, X1 - X0, COL_SEP);
  for (int i = 0; i < n; i++) {
    struct tm tmv;
    localTm(wx.hour[i].t, tmv);
    if (tmv.tm_hour % 6 == 0) tft.drawFastVLine((int)px(i), Y0, Y1 - Y0, COL_GRID);
  }

  int bwi = (int)bw - 2;
  if (bwi < 1) bwi = 1;
  for (int i = 0; i < n; i++) {
    float p = wx.hour[i].pop;
    if (isnan(p)) continue;
    if (p < 0) p = 0;
    if (p > 1) p = 1;
    int h = (int)lroundf(p * (Y1 - Y0));
    if (h > 0) tft.fillRect(X0 + (int)(i * bw) + 1, Y1 - h, bwi, h, COL_RAIN);
  }

  for (int i = 1; i < n; i++) {
    float a = wx.hour[i - 1].temp, b = wx.hour[i].temp;
    if (isnan(a) || isnan(b)) continue;
    tft.drawWideLine(px(i - 1), py(a), px(i), py(b), 2.0f, TFT_YELLOW, TFT_BLACK);
  }

  useFont(F_NONE);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Temp", X0, Y0 - 10);
  tft.setTextDatum(TR_DATUM);
  tft.drawString(String((int)tmax), X0 - 3, Y0);
  tft.setTextDatum(BR_DATUM);
  tft.drawString(String((int)tmin), X0 - 3, Y1);
  tft.setTextColor(TFT_SKYBLUE, TFT_BLACK);
  tft.setTextDatum(TR_DATUM);
  tft.drawString("Rain %", X1, Y0 - 10);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  for (int i = 0; i < n; i++) {
    struct tm tmv;
    localTm(wx.hour[i].t, tmv);
    if (tmv.tm_hour % 6 == 0) tft.drawString(String(tmv.tm_hour) + "h", (int)px(i), Y1 + 3);
  }
}

// ------------------------------------------------------------------ Page 3: 8 days
void drawDaysPage() {
  useFont(F_SMALL);
  for (int x = 80; x < SCR_W; x += 80) tft.drawFastVLine(x, 50, 186, COL_GRID);
  tft.drawFastHLine(4, 142, 312, COL_GRID);

  for (int d = 0; d < wx.nDays && d < 8; d++) {
    const WxDay& wd = wx.day[d];
    int x = (d % 4) * 80;
    int y = 48 + (d / 4) * 96;
    struct tm tmv;
    localTm(wd.t, tmv);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(d == 0 ? TFT_YELLOW : TFT_ORANGE, TFT_BLACK);
    tft.drawString(DOW_SHORT[tmv.tm_wday], x + 5, y + 3);   // today = yellow
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(TFT_SKYBLUE, TFT_BLACK);
    tft.drawString(fmtPct(wd.pop), x + 77, y + 3);
    drawBmp("/icon50/" + iconFor(wd.icon, NAN) + ".bmp", x + 1, y + 21);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(fmtTemp(wd.tMax), x + 77, y + 30);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(fmtTemp(wd.tMin), x + 77, y + 50);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString(fmtRain(wd.precipAcc), x + 40, y + 75);
  }
}

// ------------------------------------------------------------------ Page 4: Tide & sea
// Cosine interpolation between tide extremes
float tideAt(uint32_t t, bool* rising) {
  for (int i = 0; i + 1 < ha.nTide; i++) {
    uint32_t t0 = ha.tideT[i], t1 = ha.tideT[i + 1];
    if (t >= t0 && t <= t1 && t1 > t0) {
      float f  = (float)(t - t0) / (float)(t1 - t0);
      float h0 = ha.tideH[i], h1 = ha.tideH[i + 1];
      if (rising) *rising = h1 > h0;
      return h0 + (h1 - h0) * (1.0f - cosf(f * (float)M_PI)) / 2.0f;
    }
  }
  return NAN;
}

void drawHaMissing(const char* title) {
  useFont(F_SMALL);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.drawString(title, 6, 49);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  if (!HA_ENABLED) {
    tft.drawString("Home Assistant disabled", 160, 120);
    return;
  }
  tft.drawString("No Home Assistant data", 160, 115);
  if (haError.length()) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString(fitText(haError, 300), 160, 140);
  }
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(fitText(String(HA_URL), 300), 160, 170);
}

void drawTidePage() {
  if (!ha.valid || ha.nTide < 2) {
    drawHaMissing("TIDE & SEA");
    return;
  }
  uint32_t nowT = timeValid() ? (uint32_t)time(nullptr) : ha.fetched;
  bool rising = false;
  float level = tideAt(nowT, &rising);

  useFont(F_SMALL);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.drawString(TIDE_TITLE, 6, 49);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(isnan(level) ? String("--") : String(level, 2) + " m " + (rising ? "rising" : "falling"), 314, 49);

  // Chart: now - 3 h .. now + 21 h
  const int X0 = 8, X1 = 312, Y0 = 82, Y1 = 166;
  const uint32_t span = 24UL * 3600UL;
  uint32_t tStart = nowT - 3UL * 3600UL;
  float hmin = 1e9f, hmax = -1e9f;
  for (int i = 0; i < ha.nTide; i++) {
    if (ha.tideH[i] < hmin) hmin = ha.tideH[i];
    if (ha.tideH[i] > hmax) hmax = ha.tideH[i];
  }
  hmin -= 0.3f;
  hmax += 0.3f;
  auto tx = [&](uint32_t t) -> int { return X0 + (int)((float)(t - tStart) / span * (X1 - X0)); };
  auto hy = [&](float h) -> int { return Y1 - (int)((h - hmin) / (hmax - hmin) * (Y1 - Y0)); };

  int nowX = tx(nowT);
  int prevY = -1;
  for (int x = X0; x <= X1; x++) {
    uint32_t t = tStart + (uint32_t)((float)(x - X0) / (X1 - X0) * span);
    float h = tideAt(t, nullptr);
    if (isnan(h)) {
      prevY = -1;
      continue;
    }
    int y = hy(h);
    tft.drawFastVLine(x, y, Y1 - y + 1, x < nowX ? COL_SEA_PAST : COL_SEA);
    if (prevY >= 0) {
      int a = prevY < y ? prevY : y, b = prevY < y ? y : prevY;
      tft.drawFastVLine(x, a - 1, b - a + 2, COL_TIDE);
    } else {
      tft.drawFastVLine(x, y - 1, 2, COL_TIDE);
    }
    prevY = y;
  }
  tft.drawFastHLine(X0, Y1 + 1, X1 - X0, COL_SEP);

  // Hour ticks every 6 h
  useFont(F_NONE);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  for (uint32_t t = (tStart / 3600UL + 1) * 3600UL; t < tStart + span; t += 3600UL) {
    struct tm tmv;
    localTm(t, tmv);
    if (tmv.tm_hour % 6 != 0) continue;
    int x = tx(t);
    tft.drawFastVLine(x, Y1 + 2, 3, TFT_DARKGREY);
    tft.drawString(String(tmv.tm_hour) + "h", x, Y1 + 5);
  }

  // Extremes in window: dot + label
  for (int i = 0; i < ha.nTide; i++) {
    uint32_t t = ha.tideT[i];
    if (t < tStart || t > tStart + span) continue;
    int x = tx(t), y = hy(ha.tideH[i]);
    tft.fillCircle(x, y, 3, ha.tideHigh[i] ? TFT_WHITE : TFT_LIGHTGREY);
    String lbl = fmtHM(t) + " " + String(ha.tideH[i], 1) + "m";
    int lx = x;
    if (lx < 36) lx = 36;
    if (lx > 284) lx = 284;
    tft.setTextColor(ha.tideHigh[i] ? TFT_WHITE : TFT_LIGHTGREY, ha.tideHigh[i] ? TFT_BLACK : COL_SEA);
    if (ha.tideHigh[i]) {
      tft.setTextDatum(BC_DATUM);
      tft.drawString(lbl, lx, y - 6);
    } else {
      tft.setTextDatum(TC_DATUM);
      tft.drawString(lbl, lx, y + 6);
    }
  }

  // Now marker
  tft.drawFastVLine(nowX, Y0 - 12, Y1 - Y0 + 12, TFT_YELLOW);
  if (!isnan(level)) tft.fillCircle(nowX, hy(level), 4, TFT_YELLOW);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextDatum(BC_DATUM);
  tft.drawString("now", nowX, Y0 - 13);

  // Info tiles: next high, next low, waves, sea temp, moon
  int nh = -1, nl = -1;
  for (int i = 0; i < ha.nTide; i++) {
    if (ha.tideT[i] <= nowT) continue;
    if (ha.tideHigh[i] && nh < 0) nh = i;
    if (!ha.tideHigh[i] && nl < 0) nl = i;
  }
  const char* lab[5] = { "High", "Low", "Waves", "Sea", "Moon" };
  String v1[5], v2[5];
  v1[0] = nh >= 0 ? fmtHM(ha.tideT[nh]) : String("--");
  v2[0] = nh >= 0 ? String(ha.tideH[nh], 2) + " m" : String("");
  v1[1] = nl >= 0 ? fmtHM(ha.tideT[nl]) : String("--");
  v2[1] = nl >= 0 ? String(ha.tideH[nl], 2) + " m" : String("");
  v1[2] = fmtNum(ha.wave, 2, " m");
  v2[2] = isnan(ha.wave) ? String("") : String(ha.wave < 0.5f ? "calm" : (ha.wave < 1.25f ? "moderate" : "rough"));
  v1[3] = fmtTemp(ha.sst);
  v2[3] = "water";
  v1[4] = isnan(ha.moon) ? String("--") : String((int)lroundf(ha.moon)) + "%";
  v2[4] = "lit";

  useFont(F_SMALL);
  tft.drawFastHLine(6, 182, 308, COL_SEP);
  for (int k = 0; k < 5; k++) {
    int cx = 32 + k * 64;
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawString(lab[k], cx, 186);
    tft.setTextColor(k == 3 ? TFT_CYAN : TFT_WHITE, TFT_BLACK);
    tft.drawString(v1[k], cx, 204);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(v2[k], cx, 222);
  }
}

// ------------------------------------------------------------------ Page 5: Air & home
void drawAirPage() {
  if (!ha.valid) {
    drawHaMissing("AIR & HOME");
    return;
  }
  useFont(F_SMALL);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.drawString("OUTDOOR", 6, 49);
  tft.drawString("INDOOR", 166, 49);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("HA " + fmtHM(ha.fetched), 314, 49);
  tft.drawFastVLine(160, 52, 128, COL_GRID);

  String p3 = isnan(ha.p3h) ? String("--") : String(ha.p3h > 0 ? "+" : "") + String(ha.p3h, 1) + " hPa";

  struct Row {
    const char* label;
    String      value;
    uint16_t    color;
  };
  Row left[5] = {
    { "Temp",     fmtTemp(ha.outT), TFT_YELLOW },
    { "Humidity", isnan(ha.outH) ? String("--") : String((int)lroundf(ha.outH)) + "%", TFT_WHITE },
    { "Feels",    fmtTemp(ha.outF), TFT_WHITE },
    { "AQI",      isnan(ha.aqi) ? String("--") : String((int)lroundf(ha.aqi)), aqiColor(ha.aqi) },
    { "PM2.5",    isnan(ha.pm25) ? String("--") : String((int)lroundf(ha.pm25)) + " ug", pmColor(ha.pm25) },
  };
  Row right[5] = {
    { "CO2",       isnan(ha.co2) ? String("--") : String((int)lroundf(ha.co2)) + " ppm", co2Color(ha.co2) },
    { "Temp",      fmtTemp(ha.inT), TFT_YELLOW },
    { "Humidity",  isnan(ha.inH) ? String("--") : String((int)lroundf(ha.inH)) + "%", TFT_WHITE },
    { "Pres. 3h",  p3, (uint16_t)((!isnan(ha.p3h) && ha.p3h <= -2) ? TFT_ORANGE : TFT_WHITE) },
    { "PM2.5",     isnan(ha.inPm) ? String("--") : (ha.inPm < 10 ? String(ha.inPm, 1) : String((int)lroundf(ha.inPm))) + " ug", pmColor(ha.inPm) },
  };
  for (int i = 0; i < 5; i++) {
    int y = 70 + i * 21;
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(left[i].label, 6, y);
    tft.drawString(right[i].label, 166, y);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(left[i].color, TFT_BLACK);
    tft.drawString(left[i].value, 154, y);
    tft.setTextColor(right[i].color, TFT_BLACK);
    tft.drawString(right[i].value, 314, y);
  }

  tft.drawFastHLine(6, 180, 308, COL_SEP);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  String nc = strlen(ha.nowcast) ? String(ha.nowcast) : String(wx.hourlySummary);
  drawWrapped(nc, 6, 185, 308, 2, 18);

  // Alerts line (lightning only appears here when Blitzortung reports a strike nearby)
  String alert = "";
  if (!isnan(ha.lDist)) {
    alert = String("Lightning ") + (int)lroundf(ha.lDist) + " km";
    if (!isnan(ha.lAz)) alert += String(" ") + windDir8((int)ha.lAz);
  }
  if (!isnan(ha.cap) && ha.cap > 0) {
    if (alert.length()) alert += "  |  ";
    alert += String("Weather alerts: ") + (int)ha.cap;
  }
  String tmd = ha.tmd;
  tmd.toLowerCase();
  if (tmd.length() && tmd != "unavailable" && tmd != "unknown" && tmd != "none" && tmd != "0") {
    if (alert.length()) alert += "  |  ";
    alert += String(ALERT_LABEL) + ": " + ha.tmd;
  }
  tft.setTextDatum(TL_DATUM);
  if (alert.length()) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString(fitText(alert, 308), 6, 222);
  } else {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString("No weather alerts", 6, 222);
  }
}

// =====================================================================================
//                                    Animations
// =====================================================================================
bool animInit() {
  sprBase100 = new TFT_eSprite(&tft);
  sprWork100 = new TFT_eSprite(&tft);
  sprWork50  = new TFT_eSprite(&tft);
  bool ok = sprBase100->createSprite(100, 100) != nullptr;
  ok = ok && sprWork100->createSprite(100, 104) != nullptr;
  ok = ok && sprWork50->createSprite(50, 54) != nullptr;
  for (int i = 0; i < 4 && ok; i++) {
    sprBase50[i] = new TFT_eSprite(&tft);
    ok = sprBase50[i]->createSprite(50, 50) != nullptr;
  }
  if (!ok) {
    Serial.println("Not enough RAM for animations - static icons");
    TFT_eSprite* all[7] = { sprBase100, sprWork100, sprWork50, sprBase50[0], sprBase50[1], sprBase50[2], sprBase50[3] };
    for (TFT_eSprite* s : all) {
      if (s) {
        s->deleteSprite();
        delete s;
      }
    }
    sprBase100 = sprWork100 = sprWork50 = nullptr;
    for (int i = 0; i < 4; i++) sprBase50[i] = nullptr;
  }
  return ok;
}

AnimKind animKindFor(const String& icon, uint8_t* drops) {
  *drops = 0;
  if (icon == "clear-day") return A_SUN;
  if (icon == "clear-night") return A_NIGHT;
  if (icon == "partly-cloudy-night") return A_NIGHT;
  if (icon == "partly-cloudy-day" || icon == "cloudy") return A_DRIFT;
  if (icon == "rain") { *drops = 7; return A_RAIN; }
  if (icon == "lightRain") { *drops = 4; return A_RAIN; }
  if (icon == "drizzle") { *drops = 4; return A_DRIZZLE; }
  if (icon == "snow") { *drops = 6; return A_SNOW; }
  if (icon == "sleet") { *drops = 6; return A_SLEET; }
  if (icon == "hail") { *drops = 6; return A_HAIL; }
  if (icon == "thunderstorm") return A_THUNDER;
  if (icon == "fog") return A_FOG;
  if (icon == "wind") return A_WIND;
  return A_STATIC;
}

// BMP -> sprite. Rows from cropY downwards stay black (precipitation is animated instead).
bool loadIconSprite(TFT_eSprite* s, const String& path, int cropY) {
  s->fillSprite(TFT_BLACK);
  if (!LittleFS.exists(path)) return false;
  fs::File f = LittleFS.open(path, "r");
  if (!f) return false;
  uint32_t offset;
  uint16_t w, h;
  bool ok = readBmpHeader(f, offset, w, h);
  if (ok) {
    f.seek(offset);
    uint16_t padding = (4 - ((w * 3) & 3)) & 3;
    uint8_t line[w * 3 + padding];
    for (uint16_t row = 0; row < h; row++) {
      f.read(line, sizeof(line));
      int y = h - 1 - row;
      if (y >= cropY) continue;
      const uint8_t* p = line;
      for (uint16_t x = 0; x < w; x++) {
        uint8_t b = *p++, g = *p++, r = *p++;
        if (r | g | b) s->drawPixel(x, y, ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
      }
    }
  }
  f.close();
  return ok;
}

// Icon on the main page: animated if sprites are available, otherwise static BMP
void addIcon(const String& icon, int16_t x, int16_t y, uint8_t size) {
  String path = String(size == 100 ? "/icon/" : "/icon50/") + icon + ".bmp";
  uint8_t drops;
  AnimKind kind = animKindFor(icon, &drops);
  TFT_eSprite* base = nullptr;
  if (animMemOk && nAnims < MAX_ANIMS) {
    if (size == 100) base = sprBase100;
    else if (nBase50 < 4) base = sprBase50[nBase50];
  }
  if (!base || kind == A_STATIC) {
    drawBmp(path, x, y + 2);
    return;
  }
  bool precip = kind == A_RAIN || kind == A_DRIZZLE || kind == A_SNOW || kind == A_SLEET || kind == A_HAIL;
  int cropY = precip ? (size * 62) / 100 : size;
  if (!loadIconSprite(base, path, cropY)) {
    drawBmp(path, x, y + 2);
    return;
  }
  if (size == 50) nBase50++;
  Anim& a = anims[nAnims++];
  a.x     = x;
  a.y     = y;
  a.size  = size;
  a.kind  = kind;
  a.drops = drops;
  a.base  = base;
  renderAnim(a, millis());
}

void renderAnim(Anim& a, uint32_t t) {
  TFT_eSprite* w = a.size == 100 ? sprWork100 : sprWork50;
  float s  = a.size / 100.0f;
  float tt = (t % 600000UL) / 1000.0f;
  const float TWO_PI_F = 6.2831853f;
  int dx = 0, dy = 0;

  switch (a.kind) {
    case A_DRIFT:
      dx = (int)lroundf(3.0f * s * sinf(tt * TWO_PI_F / 6.0f));
      dy = (int)lroundf(1.5f * sinf(tt * TWO_PI_F / 3.0f));
      break;
    case A_FOG:
      dx = (int)lroundf(4.0f * s * sinf(tt * TWO_PI_F / 8.0f));
      break;
    case A_WIND:
      dx = (int)lroundf(3.0f * s * sinf(tt * TWO_PI_F / 1.6f));
      break;
    case A_RAIN: case A_DRIZZLE: case A_SNOW: case A_SLEET: case A_HAIL: case A_THUNDER:
      dy = (int)lroundf(1.2f * sinf(tt * TWO_PI_F / 3.0f));
      break;
    default:
      break;
  }

  w->fillSprite(TFT_BLACK);
  a.base->pushToSprite(w, dx, 2 + dy, TFT_BLACK);

  switch (a.kind) {
    case A_RAIN:
    case A_DRIZZLE:
    case A_SLEET:
    case A_SNOW:
    case A_HAIL: {
      float period = a.kind == A_DRIZZLE ? 1100.0f : (a.kind == A_SNOW ? 2400.0f : (a.kind == A_HAIL ? 520.0f : 650.0f));
      for (int i = 0; i < a.drops; i++) {
        float ph = fmodf(t / period + (float)i / a.drops + i * 0.37f, 1.0f);
        int   bx = 24 + (i * 29) % 52;
        int   px = (int)lroundf(bx * s);
        int   py = (int)lroundf((60.0f + ph * 40.0f) * s) + 2;
        bool  flake = a.kind == A_SNOW || (a.kind == A_SLEET && (i & 1));
        if (flake) {
          px += (int)lroundf(3.0f * s * sinf(ph * TWO_PI_F * 2.0f + i));
          int r = a.size == 100 ? 2 : 1;
          w->fillCircle(px, py, r, TFT_WHITE);
        } else if (a.kind == A_HAIL) {
          w->fillCircle(px, py, a.size == 100 ? 2 : 1, 0xDEFB);
        } else {
          int len = (int)lroundf((a.kind == A_DRIZZLE ? 6.0f : 9.0f) * s) + 1;
          w->drawFastVLine(px, py, len, COL_DROP);
          if (a.size == 100) w->drawFastVLine(px + 1, py, len, COL_DROP);
        }
      }
      break;
    }
    case A_THUNDER: {
      uint32_t m = t % 3600UL;
      if (m < 110 || (m > 200 && m < 260)) {
        bool left = ((t / 3600UL) & 1) != 0;
        float x0 = (left ? 24.0f : 74.0f) * s;
        float pts[5][2] = { { x0, 58 * s }, { x0 - 5 * s, 70 * s }, { x0 + 3 * s, 72 * s }, { x0 - 4 * s, 86 * s }, { x0 + 2 * s, 96 * s } };
        for (int i = 0; i < 4; i++)
          w->drawWideLine(pts[i][0], pts[i][1] + 2, pts[i + 1][0], pts[i + 1][1] + 2, a.size == 100 ? 2.5f : 1.5f, 0xFFF4, TFT_BLACK);
      }
      break;
    }
    case A_SUN: {
      for (int k = 0; k < 8; k++) {
        float ang = tt * 0.6f + k * (TWO_PI_F / 8.0f);
        float br  = 0.55f + 0.45f * sinf(tt * 2.0f + k);
        float r   = 44.0f * s;
        int   px  = (int)lroundf(50.0f * s + cosf(ang) * r);
        int   py  = (int)lroundf(50.0f * s + sinf(ang) * r) + 2;
        uint8_t c = (uint8_t)(br * 255);
        w->fillCircle(px, py, a.size == 100 ? 2 : 1, tft.color565(c, (uint8_t)(c * 0.8f), 0));
      }
      break;
    }
    case A_NIGHT: {
      const float st[5][2] = { { 12, 10 }, { 92, 8 }, { 62, 92 }, { 92, 90 }, { 30, 92 } };
      for (int i = 0; i < 5; i++) {
        float b = 0.5f + 0.5f * sinf(tt * 2.2f + i * 1.9f);
        if (b < 0.2f) continue;
        uint8_t c = (uint8_t)(b * 255);
        uint16_t col = tft.color565(c, c, (uint8_t)(c * 0.85f));
        int px = (int)lroundf(st[i][0] * s), py = (int)lroundf(st[i][1] * s) + 2;
        w->drawPixel(px, py, col);
        if (a.size == 100) {
          w->drawPixel(px - 1, py, col);
          w->drawPixel(px + 1, py, col);
          w->drawPixel(px, py - 1, col);
          w->drawPixel(px, py + 1, col);
        }
      }
      break;
    }
    default:
      break;
  }

  w->pushSprite(a.x, a.y);
}

void animTick(uint32_t ms) {
  if (!animMemOk || nAnims == 0 || page != 0 || overlayOpen) return;
  if (ms - lastFrameMs < ANIM_FRAME_MS) return;
  lastFrameMs = ms;
  for (int i = 0; i < nAnims; i++) renderAnim(anims[i], ms);
}

// =====================================================================================
//                              Log / diagnostics / web
// =====================================================================================
void logf(const char* fmt, ...) {
  char msg[LOG_LEN - 12];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  char line[LOG_LEN + 16];
  if (timeValid()) {
    struct tm tmv;
    localTm((uint32_t)time(nullptr), tmv);
    snprintf(line, sizeof(line), "%02d:%02d:%02d %s", tmv.tm_hour, tmv.tm_min, tmv.tm_sec, msg);
  } else {
    snprintf(line, sizeof(line), "+%lus %s", (unsigned long)(millis() / 1000), msg);
  }
  Serial.println(line);
  strlcpy(logBuf[logHead], line, LOG_LEN);
  logHead = (logHead + 1) % LOG_LINES;
  if (logCount < LOG_LINES) logCount++;
}

void diagBegin(FetchDiag& d) {
  memset(&d, 0, sizeof(d));
  d.at             = timeValid() ? (uint32_t)time(nullptr) : 0;
  d.contentLength  = -1;
  d.heapBefore     = ESP.getFreeHeap();
  d.maxAllocBefore = ESP.getMaxAllocHeap();
}

void diagHeaders(FetchDiag& d, HTTPClient& http) {
  d.contentLength = http.getSize();
  strlcpy(d.te, http.header("Transfer-Encoding").c_str(), sizeof(d.te));
  strlcpy(d.ce, http.header("Content-Encoding").c_str(), sizeof(d.ce));
  strlcpy(d.ctype, http.header("Content-Type").c_str(), sizeof(d.ctype));
  String calls = http.header("ratelimit-remaining");
  if (!calls.length()) calls = http.header("x-forecast-api-calls");
  strlcpy(d.calls, calls.c_str(), sizeof(d.calls));
}

static void diagToJson(JsonObject o, const FetchDiag& d) {
  o["at"]             = d.at;
  o["code"]           = d.code;
  o["content_length"] = d.contentLength;
  o["bytes_parsed"]   = d.bytes;
  o["ms"]             = d.ms;
  o["heap_before"]    = d.heapBefore;
  o["max_alloc"]      = d.maxAllocBefore;
  o["transfer_enc"]   = d.te;
  o["content_enc"]    = d.ce;
  o["content_type"]   = d.ctype;
  o["quota"]          = d.calls;
  o["head"]           = d.head;
  o["tail"]           = d.tail;
  o["error"]          = d.error;
}

static float jnum(float v) { return isnan(v) ? NAN : v; }

bool webAuth() {
  if (server.authenticate(WEB_USER, WEB_PASS)) return true;
  server.requestAuthentication();
  return false;
}

void webStatus() {
  JsonDocument d;
  d["version"]   = PROGRAM_VERSION;
  d["uptime_s"]  = millis() / 1000;
  d["heap"]      = ESP.getFreeHeap();
  d["heap_min"]  = ESP.getMinFreeHeap();
  d["max_alloc"] = ESP.getMaxAllocHeap();
  d["ip"]        = WiFi.localIP().toString();
  d["rssi"]      = WiFi.RSSI();
  d["time"]      = timeValid() ? (uint32_t)time(nullptr) : 0;
  d["local"]     = timeValid() ? fmtHM((uint32_t)time(nullptr)) : String("--:--");
  JsonObject ui = d["ui"].to<JsonObject>();
  ui["page"]         = page;
  ui["overlay"]      = overlayOpen;
  ui["rotation"]     = rotation;
  ui["day_level"]    = dayLevel;
  ui["night_level"]  = nightLevel;
  ui["night_mode"]   = nightModeOn;
  ui["night_active"] = nightActive();
  ui["backlight"]    = currentLevel;
  ui["anim_mem_ok"]  = animMemOk;
  ui["anims"]        = nAnims;
  ui["touch_swap"]   = touch.swapXY;
  ui["touch_flip"]   = touch.flip;

  JsonObject pw = d["pirate"].to<JsonObject>();
  pw["valid"]      = wx.valid;
  pw["last_ok"]    = lastFetchOk;
  pw["error"]      = lastError;
  pw["next_in_s"]  = (int32_t)(nextUpdateAt - millis()) / 1000;
  diagToJson(pw["diag"].to<JsonObject>(), diagPW);
  if (wx.valid) {
    JsonObject n = pw["now"].to<JsonObject>();
    n["t"]       = wx.now.t;
    n["summary"] = wx.now.summary;
    n["icon"]    = wx.now.icon;
    n["file"]    = iconFor(wx.now.icon, wx.now.precipInt);
    n["temp"]    = jnum(wx.now.temp);
    n["feels"]   = jnum(wx.now.feels);
    n["hum"]     = jnum(wx.now.hum);
    n["wind"]    = jnum(wx.now.wind);
    n["pop"]     = jnum(wx.now.pop);
    pw["hours"]  = wx.nHours;
    JsonArray days = pw["days"].to<JsonArray>();
    for (int i = 0; i < wx.nDays; i++) {
      JsonArray e = days.add<JsonArray>();
      e.add(wx.day[i].t);
      e.add(wx.day[i].icon);
      e.add(jnum(wx.day[i].tMax));
      e.add(jnum(wx.day[i].tMin));
      e.add(jnum(wx.day[i].pop));
    }
  }

  JsonObject h = d["ha"].to<JsonObject>();
  h["enabled"] = HA_ENABLED;
  h["url"]     = HA_URL;
  h["valid"]   = ha.valid;
  h["error"]   = haError;
  diagToJson(h["diag"].to<JsonObject>(), diagHA);
  if (ha.valid) {
    h["tides"]   = ha.nTide;
    h["wave"]    = jnum(ha.wave);
    h["sst"]     = jnum(ha.sst);
    h["out_t"]   = jnum(ha.outT);
    h["co2"]     = jnum(ha.co2);
    h["aqi"]     = jnum(ha.aqi);
    h["nowcast"] = ha.nowcast;
  }

  String out;
  serializeJsonPretty(d, out);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", out);
}

void webLog() {
  String out;
  for (int i = 0; i < logCount; i++) {
    int idx = (logHead - logCount + i + LOG_LINES) % LOG_LINES;
    out += logBuf[idx];
    out += '\n';
  }
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "text/plain", out);
}

// 24-bit BMP of the current display content, read back from the TFT
void webScreenshot() {
  const int w = SCR_W, h = SCR_H;
  const uint32_t rowBytes = w * 3;
  const uint32_t fileSize = 54 + rowBytes * h;
  uint8_t hdr[54] = { 'B', 'M' };
  auto put32 = [&](int off, uint32_t v) { hdr[off] = v; hdr[off + 1] = v >> 8; hdr[off + 2] = v >> 16; hdr[off + 3] = v >> 24; };
  put32(2, fileSize);
  put32(10, 54);
  put32(14, 40);
  put32(18, w);
  put32(22, h);
  hdr[26] = 1;
  hdr[28] = 24;
  put32(34, rowBytes * h);

  server.setContentLength(fileSize);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "image/bmp", "");
  server.sendContent((const char*)hdr, 54);

  uint16_t line[SCR_W];
  uint8_t  rgb[SCR_W * 3];
  for (int y = h - 1; y >= 0; y--) {
    tft.readRect(0, y, w, 1, line);
    for (int x = 0; x < w; x++) {
      uint16_t c = (uint16_t)((line[x] >> 8) | (line[x] << 8));   // readRect returns byte-swapped 565
      // channel order as read back from the CYD panel (verified against the display)
      rgb[x * 3]     = ((c >> 11) & 0x1F) << 3;   // B
      rgb[x * 3 + 1] = (c & 0x1F) << 3;           // G
      rgb[x * 3 + 2] = ((c >> 5) & 0x3F) << 2;    // R
    }
    server.sendContent((const char*)rgb, rowBytes);
  }
}

static const char WEB_INDEX[] = R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>CYD Weather</title><style>
body{background:#111;color:#ddd;font:14px system-ui,sans-serif;margin:16px}
h1{color:#ffd700;font-size:20px;margin:0 0 12px}
.wrap{display:flex;flex-wrap:wrap;gap:16px}
.card{background:#1b1b1b;border:1px solid #ffd70066;border-radius:10px;padding:12px}
img{width:640px;height:480px;image-rendering:pixelated;border:1px solid #333;cursor:crosshair;display:block}
button{background:#2a2a2a;color:#fff;border:1px solid #ffd700;border-radius:6px;padding:6px 10px;margin:2px;cursor:pointer}
pre{white-space:pre-wrap;font:12px ui-monospace,monospace;max-width:640px;margin:0}
.ok{color:#4caf50}.err{color:#ff5252}
</style></head><body>
<h1>CYD Weather Station</h1>
<div class="wrap">
<div class="card"><img id="shot" title="click = tap on the display">
<div><button onclick="shot()">Screenshot</button>
<button onclick="go('/api/page?n=prev')">&lt; Page</button><button onclick="go('/api/page?n=next')">Page &gt;</button>
<button onclick="go('/api/refresh')">Refresh data</button>
<button onclick="go('/api/brightness?d=-1')">Light -</button><button onclick="go('/api/brightness?d=1')">Light +</button>
<a href="/update"><button>Firmware update</button></a></div></div>
<div class="card"><div id="sum"></div><pre id="log"></pre></div>
<div class="card"><pre id="st"></pre></div>
</div>
<script>
const $=id=>document.getElementById(id);
function shot(){$('shot').src='/api/screenshot.bmp?'+Date.now();}
function go(u){fetch(u).then(()=>setTimeout(()=>{shot();load();},700));}
$('shot').onclick=e=>{const r=e.target.getBoundingClientRect();go('/api/tap?x='+Math.round((e.clientX-r.left)/2)+'&y='+Math.round((e.clientY-r.top)/2));};
function load(){
 fetch('/api/status').then(r=>r.json()).then(s=>{
  $('st').textContent=JSON.stringify(s,null,1);
  const p=s.pirate,h=s.ha;
  $('sum').innerHTML='<b>'+s.version+'</b> &middot; '+s.ip+' &middot; RSSI '+s.rssi+' &middot; heap '+s.heap+' (min '+s.heap_min+')<br>'+
   'Pirate: <span class="'+(p.last_ok?'ok':'err')+'">'+(p.last_ok?'OK':p.error)+'</span> &middot; '+p.diag.bytes_parsed+'/'+p.diag.content_length+' B in '+p.diag.ms+' ms<br>'+
   'HA: <span class="'+(h.valid&&!h.error?'ok':'err')+'">'+(h.valid&&!h.error?'OK':h.error)+'</span> &middot; '+h.diag.bytes_parsed+' B in '+h.diag.ms+' ms<br><br>';
 });
 fetch('/api/log').then(r=>r.text()).then(t=>$('log').textContent=t);
}
shot();load();setInterval(load,5000);
</script></body></html>)HTML";

static const char WEB_UPDATE[] = R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8"><title>CYD firmware update</title>
<style>body{background:#111;color:#ddd;font:14px system-ui;margin:24px}h1{color:#ffd700;font-size:20px}</style></head><body>
<h1>Firmware update</h1><p>Arduino IDE: Sketch &rarr; Export Compiled Binary, then upload <code>CYD_Weather_Landscape.ino.bin</code> (not the merged / bootloader file).</p>
<form method="POST" action="/update" enctype="multipart/form-data"><input type="file" name="fw" accept=".bin"> <input type="submit" value="Upload &amp; reboot"></form>
</body></html>)HTML";

void webIndex() {
  server.send(200, "text/html", WEB_INDEX);
}

void webInit() {
  server.on("/", HTTP_GET, webIndex);
  server.on("/api/status", HTTP_GET, webStatus);
  server.on("/api/log", HTTP_GET, webLog);
  server.on("/api/screenshot.bmp", HTTP_GET, webScreenshot);
  server.on("/api/refresh", HTTP_GET, []() {
    forceUpdate = true;
    logf("Web: refresh requested");
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/api/tap", HTTP_GET, []() {
    int x = server.arg("x").toInt(), y = server.arg("y").toInt();
    lastTouchMs = millis();
    onTap(x, y);
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/api/page", HTTP_GET, []() {
    String n = server.arg("n");
    if (n == "next") page = (page + 1) % NUM_PAGES;
    else if (n == "prev") page = (page + NUM_PAGES - 1) % NUM_PAGES;
    else page = constrain((int)n.toInt(), 0, NUM_PAGES - 1);
    lastTouchMs = millis();
    if (overlayOpen) closeOverlay();
    else redrawPage();
    server.send(200, "application/json", String("{\"page\":") + page + "}");
  });
  server.on("/api/brightness", HTTP_GET, []() {
    int dlt = server.arg("d").toInt();
    int lv  = dayLevel + dlt;
    if (server.hasArg("day")) lv = server.arg("day").toInt();
    dayLevel = (uint8_t)constrain(lv, 0, 11);
    if (server.hasArg("night")) nightLevel = (uint8_t)constrain((int)server.arg("night").toInt(), 0, 11);
    savePrefs();
    applyBacklight();
    server.send(200, "application/json", String("{\"day\":") + dayLevel + ",\"night\":" + nightLevel + "}");
  });
  server.on("/api/reboot", HTTP_GET, []() {
    if (!webAuth()) return;
    server.send(200, "text/plain", "rebooting");
    delay(300);
    ESP.restart();
  });
  if (WEB_OTA) {
    server.on("/update", HTTP_GET, []() {
      if (!webAuth()) return;
      server.send(200, "text/html", WEB_UPDATE);
    });
    server.on("/update", HTTP_POST,
      []() {
        if (!webAuth()) return;
        bool ok = !Update.hasError();
        server.sendHeader("Connection", "close");
        server.send(200, "text/plain", ok ? "Update OK - rebooting" : "Update FAILED");
        logf("OTA %s", ok ? "OK" : "failed");
        delay(500);
        if (ok) ESP.restart();
      },
      []() {
        if (!server.authenticate(WEB_USER, WEB_PASS)) return;
        HTTPUpload& up = server.upload();
        if (up.status == UPLOAD_FILE_START) {
          logf("OTA start: %s", up.filename.c_str());
          nAnims = 0;
          if (!Update.begin(UPDATE_SIZE_UNKNOWN)) logf("OTA begin failed");
        } else if (up.status == UPLOAD_FILE_WRITE) {
          if (Update.write(up.buf, up.currentSize) != up.currentSize) logf("OTA write failed");
        } else if (up.status == UPLOAD_FILE_END) {
          if (Update.end(true)) logf("OTA done: %u bytes", (unsigned)up.totalSize);
          else logf("OTA end failed");
        }
      });
  }
  server.onNotFound([]() { server.send(404, "text/plain", "not found"); });
  server.begin();
  webStarted = true;
  logf("Web server on http://%s/", WiFi.localIP().toString().c_str());
}

// =====================================================================================
//                                  Brightness menu
// =====================================================================================
bool inBtn(const Btn& b, int x, int y) {
  return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

void drawButton(const Btn& b, const String& label, bool large) {
  tft.fillRoundRect(b.x, b.y, b.w, b.h, 6, COL_BTN);
  tft.drawRoundRect(b.x, b.y, b.w, b.h, 6, TFT_GOLD);
  useFont(large ? F_LARGE : F_SMALL);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, COL_BTN);
  tft.drawString(label, b.x + b.w / 2, b.y + b.h / 2);
}

void drawOverlay() {
  tft.fillRoundRect(PANEL.x, PANEL.y, PANEL.w, PANEL.h, 8, COL_PANEL);
  tft.drawRoundRect(PANEL.x, PANEL.y, PANEL.w, PANEL.h, 8, TFT_GOLD);
  useFont(F_SMALL);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, COL_PANEL);
  tft.drawString("Display brightness", 160, 56);
  drawOverlayLevel();
  drawButton(BTN_FLIP, "Flip", false);
  drawButton(BTN_NIGHT, nightModeOn ? "Night ON" : "Night OFF", false);
  drawButton(BTN_CAL, "Calibrate", false);
  drawButton(BTN_CLOSE, "Close", false);
  drawButton(BTN_MINUS, "-", true);
  drawButton(BTN_PLUS, "+", true);
  useFont(F_NONE);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_DARKGREY, COL_PANEL);
  tft.drawString("Web: http://" + WiFi.localIP().toString() + "/", 160, 223);
}

void drawOverlayLevel() {
  uint8_t lvl = editingNight ? nightLevel : dayLevel;
  useFont(F_SMALL);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_ORANGE, COL_PANEL);
  tft.setTextPadding(290);
  char b[64];
  snprintf(b, sizeof(b), "%s %d/12   (night %02d:%02d-%02d:%02d)", editingNight ? "Night level" : "Day level",
           lvl + 1, NIGHT_START_HOUR, NIGHT_START_MINUTE, NIGHT_END_HOUR, NIGHT_END_MINUTE);
  tft.drawString(b, 160, 76);
  tft.setTextPadding(0);
  for (int i = 0; i < 12; i++) tft.fillRect(58 + i * 17, 96, 16, 20, i <= lvl ? TFT_YELLOW : COL_BAR_OFF);
}

void openOverlay() {
  overlayOpen  = true;
  editingNight = nightActive();
  drawOverlay();
}

void closeOverlay() {
  overlayOpen = false;
  savePrefs();
  if (editingNight) boostUntil = millis();
  editingNight = false;
  drawBrightnessIcon();
  redrawPage();
}

void overlayTap(int x, int y) {
  uint8_t& lvl = editingNight ? nightLevel : dayLevel;
  if (inBtn(BTN_MINUS, x, y)) {
    if (lvl > 0) { lvl--; drawOverlayLevel(); }
  } else if (inBtn(BTN_PLUS, x, y)) {
    if (lvl < 11) { lvl++; drawOverlayLevel(); }
  } else if (inBtn(BTN_NIGHT, x, y)) {
    nightModeOn  = !nightModeOn;
    editingNight = nightActive();
    drawOverlay();
  } else if (inBtn(BTN_FLIP, x, y)) {
    rotation   = rotation == 1 ? 3 : 1;
    touch.flip = rotation != calRotation;
    tft.setRotation(rotation);
    overlayOpen = false;
    savePrefs();
    fullRedraw();
  } else if (inBtn(BTN_CAL, x, y)) {
    overlayOpen = false;
    savePrefs();
    runCalibration();
    lastTouchMs = millis();
    fullRedraw();
  } else if (inBtn(BTN_CLOSE, x, y) || !inBtn(PANEL, x, y)) {
    closeOverlay();
  }
}

// =====================================================================================
//                                       Touch
// =====================================================================================
void onTap(int x, int y) {
#ifdef SERIAL_MESSAGES
  Serial.printf("Tap %d,%d\n", x, y);
#endif
  if (overlayOpen) {
    overlayTap(x, y);
    return;
  }
  if (y < CONTENT_Y) {
    openOverlay();
    return;
  }
  if (x < SCR_W / 3) page = (page + NUM_PAGES - 1) % NUM_PAGES;
  else page = (page + 1) % NUM_PAGES;
  redrawPage();
}

// Night dimmed: first touch only wakes. Returns true if the touch is consumed.
bool wakeIfDimmed(uint32_t ms) {
  if (overlayOpen || !nightActive()) return false;
  bool dimmed = (int32_t)(ms - boostUntil) >= 0;
  boostUntil = ms + TOUCH_WAKE_S * 1000UL;
  applyBacklight();
  return dimmed;
}

void handleTouch(uint32_t ms) {
  static bool     down     = false;
  static bool     consumed = false;
  static uint8_t  misses   = 0;
  static int16_t  sx = 0, sy = 0;
  static uint32_t t0 = 0;

  int16_t x, y;
  bool pressed = touch.getPoint(x, y);

  if (pressed) {
    misses      = 0;
    lastTouchMs = ms;
    if (!down) {
      down     = true;
      consumed = wakeIfDimmed(ms);
      sx = x;
      sy = y;
      t0 = ms;
    } else {
      if (nightActive() && !overlayOpen) boostUntil = ms + TOUCH_WAKE_S * 1000UL;
      if (!consumed && !overlayOpen && ms - t0 > LONG_PRESS_MS) {
        consumed    = true;
        forceUpdate = true;
        Serial.println("Long press -> refresh");
      }
    }
  } else if (down) {
    if (++misses >= 3) {
      down = false;
      if (!consumed) onTap(sx, sy);
    }
  }
}

// =====================================================================================
//                                        WiFi
// =====================================================================================
void handleWiFi(uint32_t ms) {
  if (WiFi.status() == WL_CONNECTED) return;
  if (ms - lastWifiAttempt < 15000) return;
  lastWifiAttempt = ms;
  Serial.println("WiFi reconnect...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

// =====================================================================================
//                                 Touch calibration
// =====================================================================================
bool captureRaw(int16_t& a, int16_t& b, uint32_t timeoutMs) {
  int16_t ra, rb, rz;
  uint32_t t0 = millis();
  while (touch.readRaw(ra, rb, rz)) {
    delay(10);
    if (millis() - t0 > timeoutMs) return false;
  }
  delay(150);
  while (!touch.readRaw(ra, rb, rz)) {
    delay(10);
    if (millis() - t0 > timeoutMs) return false;
  }
  long sa = 0, sb = 0;
  int n = 0;
  uint32_t t1 = millis();
  while (millis() - t1 < 400) {
    if (touch.readRaw(ra, rb, rz)) {
      sa += ra;
      sb += rb;
      n++;
    }
    delay(10);
  }
  uint8_t miss = 0;
  while (miss < 5) {
    if (touch.readRaw(ra, rb, rz)) miss = 0;
    else miss++;
    delay(10);
  }
  if (n < 5) return false;
  a = (int16_t)(sa / n);
  b = (int16_t)(sb / n);
  return true;
}

void drawCross(int16_t x, int16_t y, uint16_t color) {
  tft.drawFastHLine(x - 12, y, 25, color);
  tft.drawFastVLine(x, y - 12, 25, color);
  tft.drawCircle(x, y, 6, color);
}

bool runCalibration() {
  const int16_t PX[3] = { CYDTouch::X_REF0, CYDTouch::X_REF1, CYDTouch::X_REF0 };
  const int16_t PY[3] = { CYDTouch::Y_REF0, CYDTouch::Y_REF0, CYDTouch::Y_REF1 };
  int16_t A[3], B[3];

  for (int attempt = 0; attempt < 3; attempt++) {
    bool ok = true;
    for (int i = 0; i < 3 && ok; i++) {
      tft.fillScreen(TFT_BLACK);
      useFont(F_SMALL);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(TFT_YELLOW, TFT_BLACK);
      tft.drawString("Touch calibration", 160, 95);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.drawString("Tap the red cross precisely", 160, 120);
      tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
      tft.drawString(String("Point ") + (i + 1) + " / 3", 160, 145);
      drawCross(PX[i], PY[i], TFT_RED);
      ok = captureRaw(A[i], B[i], 30000);
      if (ok) Serial.printf("Cal point %d: raw a=%d b=%d\n", i, A[i], B[i]);
    }
    if (!ok) {
      Serial.println("Calibration timeout - keeping previous values");
      return false;
    }

    bool swap = abs(B[1] - B[0]) > abs(A[1] - A[0]);   // which raw channel follows screen X?
    int16_t rx0 = swap ? B[0] : A[0];
    int16_t rx1 = swap ? B[1] : A[1];
    int16_t ry0 = swap ? A[0] : B[0];
    int16_t ry2 = swap ? A[2] : B[2];

    if (abs(rx1 - rx0) >= 400 && abs(ry2 - ry0) >= 400) {
      touch.swapXY = swap;
      touch.xRaw0  = rx0;
      touch.xRaw1  = rx1;
      touch.yRaw0  = ry0;
      touch.yRaw1  = ry2;
      touch.flip   = false;
      calRotation  = rotation;
      saveCalibration();
      Serial.printf("Calibration saved: rot=%u swap=%d x %d..%d  y %d..%d\n", rotation, swap, rx0, rx1, ry0, ry2);

      tft.fillScreen(TFT_BLACK);
      useFont(F_SMALL);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(TFT_GREEN, TFT_BLACK);
      tft.drawString("Calibration saved", 160, 100);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.drawString("Draw to test - ends after 5 s", 160, 125);
      uint32_t last = millis();
      while (millis() - last < 5000) {
        int16_t x, y;
        if (touch.getPoint(x, y)) {
          tft.fillCircle(x, y, 2, TFT_YELLOW);
          last = millis();
        }
        delay(10);
      }
      return true;
    }

    tft.fillScreen(TFT_BLACK);
    useFont(F_SMALL);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("Points too close - again", 160, 120);
    delay(1500);
  }
  return false;
}

// =====================================================================================
//                                     Boot screen
// =====================================================================================
void drawSplash() {
  tft.fillScreen(TFT_BLACK);
  drawBmp("/icon/partly-cloudy-day.bmp", 110, 14);
  useFont(F_SMALL);
  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("CYD Weather Station", 160, 140);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.drawString("Pirate Weather + Home Assistant", 160, 162);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(PROGRAM_VERSION, 160, 184);
}

void bootMessage(const String& msg, uint8_t pct) {
  useFont(F_SMALL);
  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.setTextPadding(320);
  tft.drawString(msg, 160, 212);
  tft.setTextPadding(0);
  drawProgressBar(10, 218, 300, 15, pct, TFT_WHITE, TFT_BLUE);
}

/*
  The MIT License (MIT)
  Copyright (c) 2015 by Daniel Eichhorn - http://blog.squix.ch
  Adapted by Bodmer (TFT_eSPI / OpenWeather); landscape / Pirate Weather / Home Assistant edition 2026.

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:
  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.
  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/
