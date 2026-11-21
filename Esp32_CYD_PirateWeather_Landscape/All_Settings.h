// =====================================================================================
//  All_Settings.h  -  CYD Weather Station, landscape edition (Pirate Weather + optional Home Assistant)
// =====================================================================================
#pragma once

// ------------------------------------------------------------------ WiFi
#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"
#define WIFI_HOSTNAME   "cyd-weather"

// ------------------------------------------------------------------ Pirate Weather
// Free API key: https://pirateweather.net (free tier: 10,000 calls / month)
#define PW_API_KEY      "YOUR_PIRATE_WEATHER_KEY"
#define LATITUDE        "52.5200"     // example: Berlin
#define LONGITUDE       "13.4050"
#define PW_UNITS        "ca"          // ca = °C + km/h | si = °C + m/s | uk = °C + mph | us = °F + mph
#define PW_LANG         "en"          // display font is ASCII only
#define UPDATE_INTERVAL_MIN   10      // 10 min = approx. 4,400 calls/month
#define RETRY_INTERVAL_S      60

// ------------------------------------------------------------------ Home Assistant (optional)
// Adds two pages (tide & sea, air & home) fed by one POST /api/template request.
// Long-lived token: HA -> Profile -> Security -> Long-lived access tokens -> Create.
// Adjust the entity IDs in HA_Template.h. https URLs work as well.
#define HA_ENABLED      false
#define HA_URL          "http://homeassistant.local:8123"
#define HA_TOKEN        "YOUR_HA_LONG_LIVED_TOKEN"
#define HA_INTERVAL_S   120
#define TIDE_TITLE      "TIDE"        // e.g. "TIDE  Brighton"
#define ALERT_LABEL     "Alert"       // prefix for the text alert sensor ("tmd" field)

// ------------------------------------------------------------------ Web interface / REST API
// http://<ip>/  status page with live screenshot | /api/status | /api/log | /api/screenshot.bmp
// /api/tap?x=&y= | /api/page?n= | /api/refresh | /update (firmware upload, user/pass below)
#define WEB_ENABLED     true
#define WEB_OTA         true
#define WEB_USER        "admin"
#define WEB_PASS        "change-me"   // please change

// ------------------------------------------------------------------ Time
// POSIX TZ string, examples:
//   Germany  "CET-1CEST,M3.5.0,M10.5.0/3"   UK "GMT0BST,M3.5.0/1,M10.5.0"
//   US East  "EST5EDT,M3.2.0,M11.1.0"       Thailand "<+07>-7"
#define TZ_INFO         "CET-1CEST,M3.5.0,M10.5.0/3"
#define NTP_SERVER1     "pool.ntp.org"
#define NTP_SERVER2     "time.google.com"

// ------------------------------------------------------------------ Display
// 1 or 3 = landscape (180° apart). Can also be flipped on the device (brightness menu -> Flip).
#define SCREEN_ROTATION       1
#define ANIMATIONS            true    // animated weather icons on the main page
#define ANIM_FRAME_MS         70      // ~14 fps

// Brightness levels 0..11 (stored in NVS, these are first-boot defaults only)
#define DEFAULT_DAY_LEVEL     9
#define DEFAULT_NIGHT_LEVEL   2
#define NIGHT_MODE_DEFAULT    true
#define NIGHT_START_HOUR      22
#define NIGHT_START_MINUTE    0
#define NIGHT_END_HOUR        6
#define NIGHT_END_MINUTE      30
#define TOUCH_WAKE_S          20      // night: first touch = full brightness for this long

// ------------------------------------------------------------------ Operation
#define PAGE_TIMEOUT_S        60      // back to main page (0 = off)
#define OVERLAY_TIMEOUT_S     10
#define LONG_PRESS_MS         1500    // long press = refresh weather (+ HA) now
#define HOURLY_STEP           3       // hours between hourly tiles
#define TOUCH_Z_THRESHOLD     350

// ------------------------------------------------------------------ Texts
static const char* const DOW_SHORT[7] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
static const char* const MONTH_SHORT[12] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                             "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
