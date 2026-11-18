// Glucoled - Nightscout BG display for ESP32 + TFT_eSPI (set up for the ideaspark ESP32 1.9" ST7789
// 170x320 board via board_ideaspark_19.h; any TFT_eSPI board works - drawing uses tft.width()/height()).
// Build + flash: ./build.sh [upload]    WiFi / Nightscout settings: secrets.h (gitignored)
//
// The whole point vs the SugarPixel Mini: YOU own the HTTP timeout and retries,
// so a slow (~5s) Nightscout server never makes it give up. See HTTP_TIMEOUT_MS.
//
// Libraries (Arduino IDE -> Library Manager):
//   - TFT_eSPI        (board pins come from board_ideaspark_19.h; no library edits needed)
//   - ArduinoJson     (v7.x)
// Board package: "esp32 by Espressif Systems". Select your board, flash over USB-C.

#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <time.h>
#include "secrets.h"

// ---------------- EDIT THESE ----------------
struct WifiNet { const char* ssid; const char* pass; };
#ifdef SECRET_WIFI_NETWORKS                       // list of networks from secrets.h
const WifiNet WIFI_NETWORKS[] = { SECRET_WIFI_NETWORKS };
#else                                             // older secrets.h with a single network
const WifiNet WIFI_NETWORKS[] = { {SECRET_WIFI_SSID, SECRET_WIFI_PASS} };
#endif
const char*    NS_HOST   = SECRET_NS_HOST;
const char*    NS_TOKEN  = SECRET_NS_TOKEN;        // read-only token is enough
const char*    HOSTNAME  = "Glucoled";         // name shown in your router / Deco app
const uint32_t POLL_MS         = 60UL * 1000;   // fetch every 60s
const uint32_t HTTP_TIMEOUT_MS = 15000;         // generous - the fix the Mini lacks
const int      STALE_MIN       = 11;            // minutes old before it's flagged OLD
#ifdef SECRET_TZ
const char*    TZ_INFO         = SECRET_TZ;     // POSIX TZ string, e.g. "PST8PDT,M3.2.0,M11.1.0"
#else
const char*    TZ_INFO         = "JST-9";       // clock time zone (Japan, no DST)
#endif
// --------------------------------------------

TFT_eSPI tft = TFT_eSPI();
WiFiClientSecure tls;
WiFiMulti wifiMulti;                               // joins whichever known network is in range

int    lastSgv     = -1;
long   lastDateMs  = 0;
String lastDir     = "NONE";
uint32_t lastPoll  = 0;
int    lastClockMin = -1;                          // minute last drawn, so the clock redraws once a minute
const int CLOCK_H  = 34;                           // clock strip at the top of the screen

bool usable(const char* s) { return strlen(s) && strncmp(s, "YOUR_", 5) != 0; }

void addNetworks() {
  WiFi.setHostname(HOSTNAME);                      // must be set before WiFi starts
  WiFi.mode(WIFI_STA);
  for (const WifiNet& n : WIFI_NETWORKS)
    if (usable(n.ssid)) wifiMulti.addAP(n.ssid, n.pass);
}

// Scans, then connects to the strongest known network; on a drop (left home,
// hotspot came on) it rescans and switches to whichever one is now in range.
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  uint32_t t0 = millis();
  while (wifiMulti.run(5000) != WL_CONNECTED && millis() - t0 < 20000) delay(250);
}

bool fetch() {
  if (WiFi.status() != WL_CONNECTED) { connectWiFi(); if (WiFi.status() != WL_CONNECTED) return false; }
  tls.setInsecure();                               // skip cert checks (simple + robust)
  HTTPClient http;
  String url = String("https://") + NS_HOST + "/api/v1/entries/sgv.json?count=1";
  if (strlen(NS_TOKEN) && strncmp(NS_TOKEN, "YOUR_", 5) != 0)   // token optional: not needed
    url += String("&token=") + NS_TOKEN;                         // when the site is readable
  if (!http.begin(tls, url)) return false;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);                // <-- rides out the slow server
  int code = http.GET();
  if (code != 200) { http.end(); return false; }

  // Each entry has ~15 fields; keep only the 3 we draw, discarding the rest as it streams in.
  JsonDocument filter;
  filter[0]["sgv"] = true; filter[0]["date"] = true; filter[0]["direction"] = true;
  JsonDocument doc;                                // ArduinoJson v7
  DeserializationError err = deserializeJson(doc, http.getStream(),
                                             DeserializationOption::Filter(filter));
  http.end();
  if (err) return false;

  JsonArray arr = doc.as<JsonArray>();
  if (arr.size() < 1) return false;
  int sgv0 = arr[0]["sgv"] | -1;
  if (sgv0 < 0) return false;
  lastSgv    = sgv0;
  lastDateMs = arr[0]["date"] | 0L;
  lastDir    = String((const char*)(arr[0]["direction"] | "NONE"));
  return true;
}

long ageMinutes() {
  time_t now = time(nullptr);
  if (now < 1000000000L || lastDateMs == 0) return -1;   // NTP not ready
  return ((long)now - (lastDateMs / 1000)) / 60;
}

void drawArrow(int cx, int cy, const String& dir, uint16_t col) {
  float a = 0;
  if (dir == "DoubleUp" || dir == "SingleUp")       a = 90;
  else if (dir == "FortyFiveUp")                    a = 45;
  else if (dir == "Flat")                           a = 0;
  else if (dir == "FortyFiveDown")                  a = -45;
  else if (dir == "SingleDown" || dir == "DoubleDown") a = -90;
  float r = 22, rad = a * PI / 180.0;
  int x2 = cx + cos(rad) * r, y2 = cy - sin(rad) * r;
  int x1 = cx - cos(rad) * r, y1 = cy + sin(rad) * r;
  tft.drawWideLine(x1, y1, x2, y2, 4, col, TFT_BLACK);
  float h = 11;
  tft.fillTriangle(x2, y2,
                   x2 - (int)(cos(rad - 0.5) * h), y2 + (int)(sin(rad - 0.5) * h),
                   x2 - (int)(cos(rad + 0.5) * h), y2 + (int)(sin(rad + 0.5) * h), col);
}

// Small HH:MM clock across the top. Redraws only its own strip, so the number doesn't flicker.
void drawClock() {
  struct tm t;
  time_t now = time(nullptr);
  tft.fillRect(0, 0, tft.width(), CLOCK_H, TFT_BLACK);
  if (now < 1000000000L) { lastClockMin = -1; return; }  // NTP not ready: show nothing
  localtime_r(&now, &t);
  char buf[6];
  snprintf(buf, sizeof buf, "%02d:%02d", t.tm_hour, t.tm_min);
  tft.setTextFont(4); tft.setTextSize(1);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.drawString(buf, tft.width() / 2, 6);
  lastClockMin = t.tm_min;
}

// Just the number and the trend arrow. White when fresh, grey when the reading is
// older than STALE_MIN (or the clock isn't set yet) - no alarms, no extra text.
void render() {
  long age = ageMinutes();
  bool stale = (age < 0) || (age >= STALE_MIN);
  uint16_t col = stale ? TFT_DARKGREY : TFT_WHITE;

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(col, TFT_BLACK);
  tft.setTextFont(7);                              // 7-segment digits, 48px tall...
  tft.setTextSize(2);                              // ...doubled to 96px
  tft.setTextDatum(MR_DATUM);
  int numRight = tft.width() - 90;                 // leave room on the right for the arrow
  int cy = (tft.height() + CLOCK_H) / 2;          // centred in the space below the clock
  tft.drawString(lastSgv < 0 ? "--" : String(lastSgv), numRight, cy);
  tft.setTextSize(1);

  if (lastSgv >= 0) {
    int ax = tft.width() - 45, ay = cy;
    bool dbl = (lastDir == "DoubleUp" || lastDir == "DoubleDown");
    drawArrow(dbl ? ax - 12 : ax, ay, lastDir, col);
    if (dbl) drawArrow(ax + 12, ay, lastDir, col);  // two arrows for fast rise/fall
  }
  drawClock();
}

void setup() {
#ifdef BOARD_POWER_PIN
  pinMode(BOARD_POWER_PIN, OUTPUT);               // some boards (T-Display S3) gate screen power
  digitalWrite(BOARD_POWER_PIN, HIGH);
#endif
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(2); tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Connecting WiFi...", 10, 10);
  addNetworks();
  connectWiFi();
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");  // age uses the UTC epoch; TZ only affects the clock
  if (fetch()) render();
  lastPoll = millis();
}

void loop() {
  if (millis() - lastPoll >= POLL_MS) {
    lastPoll = millis();
    fetch();        // if it fails, we keep the last value and the age flags it OLD
    render();
  }
  time_t now = time(nullptr);                      // tick the clock when the minute changes
  if (now >= 1000000000L) { struct tm t; localtime_r(&now, &t); if (t.tm_min != lastClockMin) drawClock(); }
  delay(50);
}
