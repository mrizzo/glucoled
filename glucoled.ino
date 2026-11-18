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
#include <esp_system.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
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
const char*    FW_VERSION      = "0.3";
#ifndef BOARD_BUTTON
#define BOARD_BUTTON 0                             // BOOT button on most ESP32 boards
#endif
const uint32_t LONG_PRESS_MS   = 2000;          // hold this long to toggle Bluetooth log
const uint32_t BLE_ON_MS       = 10UL * 60 * 1000;  // Bluetooth log turns itself off after 10 min
const uint32_t DEBUG_PAGE_MS   = 2UL * 60 * 1000;   // debug page returns to the number after 2 min
// --------------------------------------------

TFT_eSPI tft = TFT_eSPI();
WiFiClientSecure tls;
WiFiMulti wifiMulti;                               // joins whichever known network is in range

int    lastSgv     = -1;
int64_t lastDateMs = 0;                            // ms since 1970: too big for a 32-bit long
String lastDir     = "NONE";
uint32_t lastPoll  = 0;
int    lastClockMin = -1;                          // minute last drawn, so the clock redraws once a minute
const int CLOCK_H  = 34;                           // clock strip at the top of the screen

// ---- diagnostics (shown on the debug page) ----
int      nsCode     = 0;                           // last HTTP code (negative = HTTPClient error)
uint32_t nsMs       = 0;                           // how long the last fetch took
uint32_t nsAt       = 0;                           // millis() of the last fetch
String   nsErr      = "";                          // why the last fetch failed ("" = ok)
uint32_t nsOk = 0, nsFail = 0;
bool     debugPage  = false;
uint32_t debugAt    = 0;

// ---------------- logging: USB serial + Bluetooth ----------------
// Never logs the token, the Nightscout host or WiFi passwords.
const int LOG_LINES = 40;                          // recent lines replayed when a phone connects
String    logRing[LOG_LINES];
int       logHead = 0, logCount = 0;

BLEServer*         bleServer = nullptr;
BLECharacteristic* bleTx     = nullptr;
BLE2902*           bleCccd   = nullptr;            // the phone's "subscribe to notifications" switch
bool     bleOn = false, bleConnected = false, bleReplay = false;
uint16_t bleConnId = 0;
uint32_t bleOnAt = 0, bleConnAt = 0;

void bleSend(const String& line) {
  if (!bleConnected || !bleTx) return;
  uint16_t mtu = bleServer->getPeerMTU(bleConnId);
  size_t chunk = (mtu > 23) ? min<size_t>(mtu - 3, 180) : 20;
  String out = line + "\n";
  for (size_t i = 0; i < out.length(); i += chunk) {
    size_t n = min(chunk, out.length() - i);
    bleTx->setValue((uint8_t*)out.c_str() + i, n);
    bleTx->notify();
    delay(4);                                      // give the stack room; avoids dropped notifies
  }
}

void logf(const char* fmt, ...) {
  char msg[160];
  va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
  char line[180];
  uint32_t s = millis() / 1000;
  snprintf(line, sizeof line, "[%lu:%02lu:%02lu] %s", s / 3600, (s / 60) % 60, s % 60, msg);
  Serial.println(line);
  logRing[logHead] = line;
  logHead = (logHead + 1) % LOG_LINES;
  if (logCount < LOG_LINES) logCount++;
  bleSend(line);
}

// Nordic UART Service: what Bluefruit Connect / nRF Toolbox / Serial Bluetooth Terminal call "UART".
#define NUS_SERVICE "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"   // phone -> board (ignored: read-only)
#define NUS_TX      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"   // board -> phone (log lines)

// Replay the recent log once the phone actually subscribes. Apps subscribe at different
// times after connecting (nRF Connect only when you tap it), so a timer after connect misses it.
class CccdCallbacks : public BLEDescriptorCallbacks {
  void onWrite(BLEDescriptor*) override {
    if (bleCccd && bleCccd->getNotifications()) { bleReplay = true; bleConnAt = millis(); }
  }
};

class BleCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*, esp_ble_gatts_cb_param_t* p) override {
    bleConnId = p->connect.conn_id; bleConnected = true;
  }
  void onDisconnect(BLEServer*) override {
    bleConnected = false;
    if (bleCccd) bleCccd->setNotifications(false);  // next phone must subscribe again (and gets the replay)
    if (bleOn) BLEDevice::startAdvertising();      // let the phone reconnect
  }
};

void drawClock();
void bleStart() {
  if (!bleServer) {                                // the stack is set up once, then left idle
    BLEDevice::init(HOSTNAME);
    bleServer = BLEDevice::createServer();
    bleServer->setCallbacks(new BleCallbacks());
    BLEService* svc = bleServer->createService(NUS_SERVICE);
    bleTx = svc->createCharacteristic(NUS_TX, BLECharacteristic::PROPERTY_NOTIFY);
    bleCccd = new BLE2902();
    bleCccd->setCallbacks(new CccdCallbacks());
    bleTx->addDescriptor(bleCccd);
    svc->createCharacteristic(NUS_RX, BLECharacteristic::PROPERTY_WRITE);
    svc->start();
    BLEDevice::getAdvertising()->addServiceUUID(NUS_SERVICE);
  }
  BLEDevice::startAdvertising();
  bleOn = true; bleOnAt = millis();
  logf("ble: on for %lu min, advertising as \"%s\"", BLE_ON_MS / 60000, HOSTNAME);
  if (!debugPage) drawClock();
}

void bleStop() {
  if (!bleOn) return;
  bleOn = false;
  BLEDevice::stopAdvertising();
  if (bleConnected) bleServer->disconnect(bleConnId);
  logf("ble: off");
  if (!debugPage) drawClock();
}

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
  logf("wifi: connecting");
  while (wifiMulti.run(5000) != WL_CONNECTED && millis() - t0 < 20000) delay(250);
  if (WiFi.status() == WL_CONNECTED)
    logf("wifi: joined \"%s\" rssi %d ip %s in %lu ms", WiFi.SSID().c_str(), WiFi.RSSI(),
         WiFi.localIP().toString().c_str(), millis() - t0);
  else
    logf("wifi: no known network after %lu ms", millis() - t0);
}

bool fetchOnce();
bool fetch() {
  uint32_t t0 = millis();
  nsCode = 0; nsErr = "";
  bool ok = fetchOnce();
  if (!ok && WiFi.status() == WL_CONNECTED) {      // one quick retry: a single blip shouldn't cost a minute
    logf("ns: retry after: %s", nsErr.c_str());
    delay(2000);
    nsCode = 0; nsErr = "";
    ok = fetchOnce();
  }
  nsMs = millis() - t0; nsAt = millis();
  if (ok) {
    nsOk++;
    long age = ageMinutes();
    logf("ns: %d in %lu ms -> %d %s, age %ld min", nsCode, nsMs, lastSgv, lastDir.c_str(), age);
  } else {
    nsFail++;
    logf("ns: FAIL after %lu ms: %s (keeping last value)", nsMs, nsErr.c_str());
  }
  return ok;
}

bool fetchOnce() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) { nsErr = "no wifi"; return false; }
  }
  tls.setInsecure();                               // skip cert checks (simple + robust)
  HTTPClient http;
  String url = String("https://") + NS_HOST + "/api/v1/entries/sgv.json?count=1";
  if (strlen(NS_TOKEN) && strncmp(NS_TOKEN, "YOUR_", 5) != 0)   // token optional: not needed
    url += String("&token=") + NS_TOKEN;                         // when the site is readable
  if (!http.begin(tls, url)) { nsErr = "bad url"; return false; }
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);                // <-- rides out the slow server
  int code = http.GET();
  nsCode = code;
  if (code != 200) {
    nsErr = code < 0 ? HTTPClient::errorToString(code) : String("HTTP ") + code;
    http.end(); return false;
  }

  // Each entry has ~15 fields; keep only the 3 we draw, discarding the rest as it streams in.
  JsonDocument filter;
  filter[0]["sgv"] = true; filter[0]["date"] = true; filter[0]["direction"] = true;
  // Read the whole body first (a few hundred bytes). Parsing straight off the stream failed
  // ~1 in 5 times with IncompleteInput: a slow TLS packet or chunked encoding ended it early.
  String body = http.getString();
  http.end();
  JsonDocument doc;                                // ArduinoJson v7
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) { nsErr = String("json: ") + err.c_str(); return false; }

  JsonArray arr = doc.as<JsonArray>();
  if (arr.size() < 1) { nsErr = "no entries"; return false; }
  int sgv0 = arr[0]["sgv"] | -1;
  if (sgv0 < 0) { nsErr = "no sgv"; return false; }
  lastSgv    = sgv0;
  lastDateMs = arr[0]["date"] | (int64_t)0;
  lastDir    = String((const char*)(arr[0]["direction"] | "NONE"));
  return true;
}

long ageMinutes() {
  time_t now = time(nullptr);
  if (now < 1000000000L || lastDateMs == 0) return -1;   // NTP not ready
  return (long)(((int64_t)now - lastDateMs / 1000) / 60);
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
  if (bleOn) {                                     // small reminder that Bluetooth is advertising
    tft.setTextFont(2); tft.setTextSize(1); tft.setTextDatum(TL_DATUM);
    tft.setTextColor(bleConnected ? TFT_CYAN : TFT_BLUE, TFT_BLACK);
    tft.drawString("BT", 6, 6);
  }
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

// ---------------- debug page ----------------
String ascii(const String& s) {                    // the screen font has no emoji: show them as ?
  String o; bool q = false;
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    if (c < 0x80) { o += (char)c; q = false; }
    else if (!q) { o += '?'; q = true; }           // one ? per run of non-ASCII bytes
  }
  return o;
}

String ago(uint32_t ms) {
  uint32_t s = ms / 1000;
  if (s < 60) return String(s) + "s";
  if (s < 3600) return String(s / 60) + "m" + String(s % 60) + "s";
  return String(s / 3600) + "h" + String((s / 60) % 60) + "m";
}

void drawDebug() {
  const int lh = 18;                               // font 2 is 16 px; 9 lines fit in 170
  int y = 2, w = tft.width() - 8;
  tft.setTextFont(2); tft.setTextSize(1); tft.setTextDatum(TL_DATUM);
  tft.setTextPadding(w);                           // overwrite old text without clearing (no flicker)
  auto line = [&](uint16_t c, const String& t) { tft.setTextColor(c, TFT_BLACK); tft.drawString(t, 4, y); y += lh; };

  line(TFT_YELLOW, String("Glucoled ") + FW_VERSION + "   debug  (press to close)");
  if (WiFi.status() == WL_CONNECTED) {
    line(TFT_WHITE, "WiFi  " + ascii(WiFi.SSID()) + "  " + WiFi.RSSI() + " dBm");
    line(TFT_WHITE, "IP    " + WiFi.localIP().toString());
  } else {
    line(TFT_RED, "WiFi  not connected");
    line(TFT_WHITE, "");
  }
  if (nsAt == 0) line(TFT_WHITE, "NS    no fetch yet");
  else line(nsErr.length() ? TFT_RED : TFT_GREEN,
            "NS    " + (nsErr.length() ? nsErr : String(nsCode)) + " in " + String(nsMs / 1000.0, 1) +
            "s, " + ago(millis() - nsAt) + " ago");
  line(TFT_WHITE, "Fetches  " + String(nsOk) + " ok / " + String(nsFail) + " failed");
  long age = ageMinutes();
  line(TFT_WHITE, "Reading  " + (lastSgv < 0 ? String("--") : String(lastSgv) + " " + lastDir) +
                  (age < 0 ? String("") : ", " + String(age) + " min old"));
  line(TFT_WHITE, "Up " + ago(millis()) + "   heap " + String(ESP.getFreeHeap() / 1024) + " KB");
  String bt = !bleOn ? String("off (hold button 2s)")
            : bleConnected ? "connected"
            : "advertising, " + ago(BLE_ON_MS - (millis() - bleOnAt)) + " left";
  line(bleOn ? TFT_CYAN : TFT_DARKGREY, "BT    " + bt);
  tft.setTextPadding(0);
}

void showDebug(bool on) {
  debugPage = on; debugAt = millis();
  tft.fillScreen(TFT_BLACK);
  if (on) drawDebug(); else render();
}

// Short press: debug page on/off. Hold 2 s: Bluetooth log on/off.
bool btnDown = false, btnLong = false;
uint32_t btnT0 = 0;
void pollButton() {
  bool down = digitalRead(BOARD_BUTTON) == LOW;
  uint32_t now = millis();
  if (down && !btnDown) { btnDown = true; btnLong = false; btnT0 = now; }
  else if (down && !btnLong && now - btnT0 >= LONG_PRESS_MS) { btnLong = true; bleOn ? bleStop() : bleStart(); }
  else if (!down && btnDown) {
    btnDown = false;
    if (!btnLong && now - btnT0 >= 30) showDebug(!debugPage);   // 30 ms debounce
  }
}

const char* resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW:      return "software";
    case ESP_RST_PANIC:   return "crash";
    case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_EXT:     return "reset pin";
    default:              return "other";
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BOARD_BUTTON, INPUT_PULLUP);
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
  logf("boot: Glucoled %s on %s, reset: %s", FW_VERSION, ARDUINO_BOARD, resetReason());
  addNetworks();
  connectWiFi();
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");  // age uses the UTC epoch; TZ only affects the clock
  fetch();
  render();                                        // shows "--" if the first fetch failed
  lastPoll = millis();
}

uint32_t lastDebugDraw = 0;
bool timeLogged = false;
wl_status_t lastWifi = WL_IDLE_STATUS;

void loop() {
  pollButton();
  if (millis() - lastPoll >= POLL_MS) {
    lastPoll = millis();
    fetch();        // if it fails, we keep the last value and the age flags it OLD
    if (!debugPage) render();
  }

  time_t now = time(nullptr);
  if (now >= 1000000000L && !timeLogged) {
    timeLogged = true;
    char b[32]; struct tm t; localtime_r(&now, &t); strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &t);
    logf("time: synced, %s (%s)", b, TZ_INFO);
  }
  wl_status_t ws = WiFi.status();                  // log drops/reconnects between fetches
  if (ws != lastWifi) {
    if (ws == WL_CONNECTED) logf("wifi: up, \"%s\" rssi %d", WiFi.SSID().c_str(), WiFi.RSSI());
    else if (lastWifi == WL_CONNECTED) logf("wifi: dropped (status %d)", ws);
    lastWifi = ws;
  }

  if (bleConnected && bleReplay && millis() - bleConnAt > 200) {   // just after the phone subscribes
    bleReplay = false;
    for (int i = 0; i < logCount; i++) bleSend(logRing[(logHead - logCount + i + LOG_LINES) % LOG_LINES]);
    logf("ble: phone connected (above: last %d log lines)", logCount);
  }
  if (bleOn && !bleConnected && millis() - bleOnAt >= BLE_ON_MS) bleStop();

  if (debugPage) {
    if (millis() - debugAt >= DEBUG_PAGE_MS) showDebug(false);
    else if (millis() - lastDebugDraw >= 1000) { lastDebugDraw = millis(); drawDebug(); }
  } else if (now >= 1000000000L) {                 // tick the clock when the minute changes
    struct tm t; localtime_r(&now, &t);
    if (t.tm_min != lastClockMin) drawClock();
  }
  delay(20);
}
