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
#include <FS.h>
#include <WebServer.h>                             // setup page only (setup_page.ino); before TFT_eSPI
#include <DNSServer.h>
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
#include <Preferences.h>
#include <qrcode.h>                                // ESP-IDF QR encoder, part of the ESP32 core
#if __has_include("secrets.h") && !defined(GLUCOLED_NO_SECRETS)   // optional developer defaults;
                                                   // never in public builds (build.sh NOSECRETS=1)
#include "secrets.h"                               // Normally everything is set on the setup page.
#endif
#include "dexcom_roots.h"

// ---------------- settings ----------------
// Saved on the board by the setup page (hold the button 6 s). Until something is saved there,
// the defaults from secrets.h (if any) are used.
const int MAX_NETS = 4;
struct Config {
  String ssid[MAX_NETS], pass[MAX_NETS];           // WiFi networks, joined strongest-first
  String nsHost, nsToken;                          // Nightscout (token optional, read-only is enough)
  String dexUser, dexPass, dexRegion = "us";       // Dexcom Share: the sensor WEARER's login
  String tz = "JST-9";                             // POSIX time zone for the clock
} cfg;

struct WifiNet { const char* ssid; const char* pass; };
#if defined(SECRET_WIFI_NETWORKS)
const WifiNet DEFAULT_NETS[] = { SECRET_WIFI_NETWORKS };
#elif defined(SECRET_WIFI_SSID)                   // older secrets.h with a single network
const WifiNet DEFAULT_NETS[] = { {SECRET_WIFI_SSID, SECRET_WIFI_PASS} };
#else
const WifiNet DEFAULT_NETS[] = { {"", ""} };
#endif

bool usable(const String& s) { return s.length() && !s.startsWith("YOUR_"); }

bool loadConfig() {                                // true if the setup page has saved settings
  Preferences p;
  bool saved = p.begin("glucoled", true) && p.getBool("saved", false);
  if (saved) {
    for (int i = 0; i < MAX_NETS; i++) {
      cfg.ssid[i] = p.getString(("s" + String(i)).c_str(), "");
      cfg.pass[i] = p.getString(("p" + String(i)).c_str(), "");
    }
    cfg.nsHost = p.getString("nsHost", ""); cfg.nsToken = p.getString("nsToken", "");
    cfg.dexUser = p.getString("dexUser", ""); cfg.dexPass = p.getString("dexPass", "");
    cfg.dexRegion = p.getString("dexRegion", "us"); cfg.tz = p.getString("tz", "JST-9");
  } else {
    int n = 0;
    for (const WifiNet& w : DEFAULT_NETS)
      if (n < MAX_NETS && usable(w.ssid)) { cfg.ssid[n] = w.ssid; cfg.pass[n] = w.pass; n++; }
#ifdef SECRET_NS_HOST
    cfg.nsHost = SECRET_NS_HOST;
#endif
#ifdef SECRET_NS_TOKEN
    cfg.nsToken = SECRET_NS_TOKEN;
#endif
#if defined(SECRET_DEXCOM_USER) && defined(SECRET_DEXCOM_PASS)
    cfg.dexUser = SECRET_DEXCOM_USER; cfg.dexPass = SECRET_DEXCOM_PASS;
#endif
#ifdef SECRET_DEXCOM_REGION
    cfg.dexRegion = SECRET_DEXCOM_REGION;
#endif
#ifdef SECRET_TZ
    cfg.tz = SECRET_TZ;
#endif
  }
  p.end();
  return saved;
}

void saveConfig() {
  Preferences p;
  p.begin("glucoled", false);
  for (int i = 0; i < MAX_NETS; i++) {
    p.putString(("s" + String(i)).c_str(), cfg.ssid[i]);
    p.putString(("p" + String(i)).c_str(), cfg.pass[i]);
  }
  p.putString("nsHost", cfg.nsHost); p.putString("nsToken", cfg.nsToken);
  p.putString("dexUser", cfg.dexUser); p.putString("dexPass", cfg.dexPass);
  p.putString("dexRegion", cfg.dexRegion); p.putString("tz", cfg.tz);
  p.putBool("saved", true);
  p.end();
}

void eraseConfig() { Preferences p; p.begin("glucoled", false); p.clear(); p.end(); }

bool haveWifi() { for (int i = 0; i < MAX_NETS; i++) if (usable(cfg.ssid[i])) return true; return false; }

const char*    HOSTNAME  = "Glucoled";         // name shown in your router / Deco app
const uint32_t POLL_MS         = 60UL * 1000;   // fetch every 60s
const uint32_t HTTP_TIMEOUT_MS = 15000;         // generous - the fix the Mini lacks
const int      STALE_MIN       = 11;            // minutes old before it's flagged OLD
const char*    FW_VERSION      = "0.5";
#ifndef BOARD_BUTTON
#define BOARD_BUTTON 0                             // BOOT button on most ESP32 boards
#endif
const uint32_t LONG_PRESS_MS   = 2000;          // hold 2 s, release: Bluetooth log on/off
const uint32_t SETUP_PRESS_MS  = 6000;          // hold 6 s: setup page
const uint32_t BLE_ON_MS       = 10UL * 60 * 1000;  // Bluetooth log turns itself off after 10 min
const uint32_t DEBUG_PAGE_MS   = 2UL * 60 * 1000;   // debug page returns to the number after 2 min
// --------------------------------------------

TFT_eSPI tft = TFT_eSPI();
WiFiClientSecure tls;
WiFiMulti wifiMulti;                               // joins whichever known network is in range

int    lastSgv     = -1;
int64_t lastDateMs = 0;                            // ms since 1970: too big for a 32-bit long
String lastDir     = "NONE";
const char* lastSrc = "";                          // where the shown reading came from: "NS" or "D"
struct Reading { int sgv = -1; int64_t dateMs = 0; String dir = "NONE"; };
Reading got;                                       // filled by nsFetch()/dexFetch()
uint32_t lastPoll  = 0;
int    lastClockMin = -1;                          // minute last drawn, so the clock redraws once a minute
const int CLOCK_H  = 34;                           // clock strip at the top of the screen

// ---- diagnostics (shown on the debug page) ----
int      nsCode     = 0;                           // scratch: HTTP code of the source being fetched
String   nsErr      = "";                          // scratch: why it failed ("" = ok)
struct SrcStat { int code = 0; uint32_t ms = 0, at = 0, ok = 0, fail = 0; String err; };
SrcStat  nsStat, dexStat;                          // per source, for the debug page
#ifndef DEX_BACKUP_MIN
#define DEX_BACKUP_MIN 6                           // ask Dexcom when Nightscout's reading is this old
#endif
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

bool useDexcom() { return usable(cfg.dexUser) && usable(cfg.dexPass); }
bool useNs()     { return usable(cfg.nsHost); }

void addNetworks() {
  WiFi.setHostname(HOSTNAME);                      // must be set before WiFi starts
  WiFi.mode(WIFI_STA);
  for (int i = 0; i < MAX_NETS; i++)
    if (usable(cfg.ssid[i])) wifiMulti.addAP(cfg.ssid[i].c_str(), cfg.pass[i].c_str());
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

bool nsFetch();
bool dexFetch();
long ageOf(int64_t dateMs);
bool fetchNoRetry = false;                         // set for errors a retry can't fix (bad login, no clock)

// Fetch one source into `got`, with one quick retry, and record its stats.
bool runSource(bool dex) {
  SrcStat& st = dex ? dexStat : nsStat;
  const char* tag = dex ? "dex" : "ns";
  uint32_t t0 = millis();
  got = Reading(); nsCode = 0; nsErr = ""; fetchNoRetry = false;
  bool ok = dex ? dexFetch() : nsFetch();
  if (!ok && !fetchNoRetry && WiFi.status() == WL_CONNECTED) {  // a blip shouldn't cost a minute
    logf("%s: retry after: %s", tag, nsErr.c_str());
    delay(2000);
    got = Reading(); nsCode = 0; nsErr = ""; fetchNoRetry = false;
    ok = dex ? dexFetch() : nsFetch();
  }
  st.ms = millis() - t0; st.at = millis(); st.code = nsCode; st.err = ok ? "" : nsErr;
  if (ok) {
    st.ok++;
    logf("%s: %d in %lu ms -> %d %s, age %ld min", tag, nsCode, st.ms, got.sgv, got.dir.c_str(), ageOf(got.dateMs));
  } else {
    st.fail++;
    logf("%s: FAIL after %lu ms: %s", tag, st.ms, nsErr.c_str());
  }
  return ok;
}

// Nightscout first (fast when the uploader is working). If its reading is missing or
// DEX_BACKUP_MIN+ minutes old (uploader missed one), also ask Dexcom, and show the newer.
// With only one source set, just that one is used.
bool fetch() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) { logf("fetch: no wifi"); return false; }
  }
  if (!useNs() && !useDexcom()) { logf("fetch: no data source set (hold the button 6 s for setup)"); return false; }

  Reading ns, dx;
  bool nsOk = false, dxOk = false;
  if (useNs() && (nsOk = runSource(false))) ns = got;
  long nsAge = nsOk ? ageOf(ns.dateMs) : -1;
  if (useDexcom() && (!nsOk || nsAge < 0 || nsAge >= DEX_BACKUP_MIN)) {
    if (useNs()) logf("dex: asking Dexcom, Nightscout %s", nsOk ? (String("reading is ") + nsAge + " min old").c_str() : "failed");
    if ((dxOk = runSource(true))) dx = got;
  }

  const Reading* best = nullptr; const char* src = "";
  if (nsOk) { best = &ns; src = "NS"; }
  // Same reading from both sources has slightly different timestamps, so Dexcom must be a
  // minute newer to win; otherwise prefer Nightscout.
  if (dxOk && (!best || dx.dateMs > best->dateMs + 60000)) { best = &dx; src = "D"; }
  if (!best) { logf("fetch: no source answered (keeping last value)"); return false; }
  if (best->dateMs >= lastDateMs) {                // never step back to an older reading
    if (strcmp(src, lastSrc)) logf("show: now from %s", src);
    lastSgv = best->sgv; lastDateMs = best->dateMs; lastDir = best->dir; lastSrc = src;
  }
  return true;
}

// ---------------- Nightscout ----------------
bool nsFetch() {
  tls.setInsecure();                               // skip cert checks (simple + robust)
  HTTPClient http;
  String url = "https://" + cfg.nsHost + "/api/v1/entries/sgv.json?count=1";
  if (usable(cfg.nsToken))                         // token optional: not needed when the site is readable
    url += "&token=" + cfg.nsToken;
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
  got.sgv    = sgv0;
  got.dateMs = arr[0]["date"] | (int64_t)0;
  got.dir    = String((const char*)(arr[0]["direction"] | "NONE"));
  return true;
}

// ---------------- Dexcom Share ----------------
// The unofficial API the Dexcom Follow app uses (same flow as pydexcom): authenticate -> account
// ID, log in -> session ID, then read the latest value. The session is reused until it expires.
// This sends the Dexcom PASSWORD, so unlike Nightscout the server certificate IS verified.
String   dexAccountId, dexSessionId;               // never logged
bool     dexLoginLocked  = false;                  // login refused: stop trying (avoids locking the account)
uint32_t dexLoginRetryAt = 0;

const char* dexHost() {
  if (cfg.dexRegion == "jp")  return "share.dexcom.jp";
  if (cfg.dexRegion == "ous") return "shareous1.dexcom.com";
  return "share2.dexcom.com";
}
const char* dexAppId() {                           // Japan has its own application ID
  return cfg.dexRegion == "jp" ? "d8665ade-9673-4e27-9ff6-92db4ce13d13"
                                   : "d89443d2-327c-4a6f-89e5-496bbb0317db";
}

int dexPost(const String& path, const String& body, String& out) {
  WiFiClientSecure c;
  c.setCACert(DEXCOM_ROOT_CAS);                    // verify: we're sending a password
  HTTPClient http;
  out = "";
  if (!http.begin(c, String("https://") + dexHost() + "/ShareWebServices/Services/" + path)) return -100;
  // Handshake takes ~1 s. Right after a cold boot the first connect sometimes stalls (seen
  // only while the clock was just set), so give up sooner and let the retry handle it.
  http.setConnectTimeout(8000);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  int code = http.POST(body);
  if (code > 0) out = http.getString();
  else {                                           // log why TLS/connect failed (cert, DNS, memory...)
    char e[100] = "";
    int te = c.lastError(e, sizeof e);
    logf("dex: connect to %s failed: %s (tls %d: %s, heap %u)", dexHost(),
         HTTPClient::errorToString(code).c_str(), te, e, ESP.getFreeHeap());
  }
  http.end();
  return code;
}

String jsonString(const String& body) {            // Share returns IDs as a bare JSON string
  JsonDocument d;
  if (deserializeJson(d, body)) return "";
  const char* v = d.as<const char*>();
  return v ? String(v) : "";
}
String dexErrCode(const String& body) {            // {"Code":"SessionIdNotFound","Message":...}
  JsonDocument d;
  if (deserializeJson(d, body) || !d.is<JsonObject>()) return "";
  return String((const char*)(d["Code"] | ""));
}
bool validId(const String& id) { return id.length() == 36 && !id.startsWith("00000000-"); }

bool dexLoginFailed(int code, const String& out) {
  String c = dexErrCode(out);
  nsCode = code; fetchNoRetry = true;
  if (code == 500 && c.length()) {                 // Dexcom said no: wrong user/password/region, or locked
    dexLoginLocked = true;
    nsErr = "Dexcom login refused (" + c + "). Check user/password/region on the setup page";
  } else if (code == 200) {                        // all-zero ID: account exists but can't share
    dexLoginLocked = true;
    nsErr = "Dexcom login gave no session: wrong region, or Share is off";
  } else if (code < 0) {                           // never reached Dexcom: can't affect the account,
    nsErr = "Dexcom unreachable (" + HTTPClient::errorToString(code) + ")";
    fetchNoRetry = false;                          // so the normal quick retry is fine
  } else {                                         // server trouble: try again later
    dexLoginRetryAt = millis() + 5UL * 60 * 1000;
    nsErr = "Dexcom login failed (" + (code < 0 ? HTTPClient::errorToString(code) : String("HTTP ") + code) +
            "), retry in 5 min";
  }
  return false;
}

bool dexLogin() {
  if (dexLoginLocked) { nsErr = "Dexcom login refused earlier. Fix it on the setup page"; fetchNoRetry = true; return false; }
  if ((int32_t)(millis() - dexLoginRetryAt) < 0 && dexLoginRetryAt) { nsErr = "Dexcom login: waiting to retry"; fetchNoRetry = true; return false; }
  String body, out;
  JsonDocument req;
  if (!validId(dexAccountId)) {
    req["accountName"] = cfg.dexUser; req["password"] = cfg.dexPass; req["applicationId"] = dexAppId();
    serializeJson(req, body);
    int code = dexPost("General/AuthenticatePublisherAccount", body, out);
    dexAccountId = code == 200 ? jsonString(out) : "";
    if (!validId(dexAccountId)) { dexAccountId = ""; return dexLoginFailed(code, out); }
  }
  req.clear(); body = "";
  req["accountId"] = dexAccountId; req["password"] = cfg.dexPass; req["applicationId"] = dexAppId();
  serializeJson(req, body);
  int code = dexPost("General/LoginPublisherAccountById", body, out);
  String sid = code == 200 ? jsonString(out) : "";
  if (!validId(sid)) return dexLoginFailed(code, out);
  dexSessionId = sid;
  logf("dex: logged in (%s)", dexHost());
  return true;
}

bool dexFetch() {
  if (time(nullptr) < 1000000000L) {               // certificate dates can't be checked without a clock
    nsErr = "waiting for clock"; fetchNoRetry = true; return false;
  }
  for (int attempt = 0; attempt < 2; attempt++) {
    if (!validId(dexSessionId) && !dexLogin()) return false;
    String out;
    // Latest value from the last 24 h, so an old reading still shows (grey) with its real age.
    int code = dexPost("Publisher/ReadPublisherLatestGlucoseValues?sessionId=" + dexSessionId +
                       "&minutes=1440&maxCount=1", "{}", out);
    nsCode = code;
    if (code == 200) {
      JsonDocument doc;
      if (deserializeJson(doc, out)) { nsErr = "json"; return false; }
      JsonArray a = doc.as<JsonArray>();
      if (a.size() < 1) { nsErr = "no reading in 24 h (is Share on?)"; return false; }
      int v = a[0]["Value"] | -1;
      const char* wt = a[0]["WT"] | "";            // "Date(1763430000000)" = UTC ms
      const char* p = strchr(wt, '(');
      int64_t ms = p ? strtoll(p + 1, nullptr, 10) : 0;
      if (v < 0 || ms == 0) { nsErr = "unexpected reading format"; return false; }
      got.sgv = v; got.dateMs = ms;
      got.dir = String((const char*)(a[0]["Trend"] | "NONE"));   // same names as Nightscout
      return true;
    }
    String c = dexErrCode(out);
    if (attempt == 0 && (c == "SessionIdNotFound" || c == "SessionNotValid")) {
      logf("dex: session expired, logging in again");
      dexSessionId = "";
      continue;
    }
    nsErr = code < 0 ? HTTPClient::errorToString(code) : "HTTP " + String(code) + (c.length() ? " " + c : "");
    return false;
  }
  return false;
}

long ageOf(int64_t dateMs) {
  time_t now = time(nullptr);
  if (now < 1000000000L || dateMs == 0) return -1;      // NTP not ready
  return (long)(((int64_t)now - dateMs / 1000) / 60);
}
long ageMinutes() { return ageOf(lastDateMs); }

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
  if (lastSrc[0]) {                                // where the number came from: NS or D (Dexcom)
    tft.setTextFont(2); tft.setTextSize(1); tft.setTextDatum(TR_DATUM);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString(lastSrc, tft.width() - 6, 6);
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

// Range colours, as in Loop Follow's time-in-range chart. Override the cut-offs in secrets.h.
#ifndef RANGE_VERY_LOW
#define RANGE_VERY_LOW  54     // below: very low
#endif
#ifndef RANGE_LOW
#define RANGE_LOW       70     // below: low
#endif
#ifndef RANGE_HIGH
#define RANGE_HIGH      180    // above: high
#endif
#ifndef RANGE_VERY_HIGH
#define RANGE_VERY_HIGH 250    // above: very high
#endif
uint16_t rangeColour(int sgv) {
  if (sgv < RANGE_VERY_LOW)   return tft.color565(255, 64, 64);   // very low: bright red
  if (sgv < RANGE_LOW)        return tft.color565(200, 70, 70);   // low: deeper red
  if (sgv <= RANGE_HIGH)      return tft.color565(103, 204, 99);  // in range: green
  if (sgv <= RANGE_VERY_HIGH) return tft.color565(247, 216, 74);  // high: yellow
  return tft.color565(240, 150, 76);                              // very high: orange
}

// Just the number and the trend arrow, coloured by range. Grey when the reading is
// older than STALE_MIN (or the clock isn't set yet) - no alarms, no extra text.
void render() {
  long age = ageMinutes();
  bool stale = (age < 0) || (age >= STALE_MIN);
  uint16_t col = (stale || lastSgv < 0) ? TFT_DARKGREY : rangeColour(lastSgv);

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(col, TFT_BLACK);
  tft.setTextFont(7);                              // 7-segment digits, 48px tall...
  tft.setTextSize(2);                              // ...doubled to 96px
  tft.setTextDatum(MR_DATUM);
  int numRight = tft.width() - 90;                 // leave room on the right for the arrow
  int cy = (tft.height() + CLOCK_H) / 2;          // centred in the space below the clock
  tft.drawString(lastSgv < 0 ? "--" : String(lastSgv), numRight, cy);
  tft.setTextSize(1);

  static const char* KNOWN[] = {"DoubleUp", "SingleUp", "FortyFiveUp", "Flat",
                                 "FortyFiveDown", "SingleDown", "DoubleDown"};
  bool knownDir = false;
  for (const char* k : KNOWN) if (lastDir == k) knownDir = true;
  if (lastSgv >= 0 && knownDir) {                  // no arrow when the trend is unknown
    int ax = tft.width() - 45, ay = cy;
    bool dbl = (lastDir == "DoubleUp" || lastDir == "DoubleDown");
    drawArrow(dbl ? ax - 12 : ax, ay, lastDir, col);
    if (dbl) drawArrow(ax + 12, ay, lastDir, col);  // two arrows for fast rise/fall
  }
  if (WiFi.status() != WL_CONNECTED || (!useNs() && !useDexcom())) {   // say how to fix it
    tft.setTextFont(2); tft.setTextSize(1); tft.setTextDatum(BC_DATUM);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString(WiFi.status() != WL_CONNECTED ? "No WiFi. Hold the button 6 s for setup"
                                                 : "No data source. Hold the button 6 s for setup",
                   tft.width() / 2, tft.height() - 2);
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
  auto srcLine = [&](const char* tag, bool on, const SrcStat& st, const char* idle) {
    if (!on) line(TFT_DARKGREY, String(tag) + "not set");
    else if (st.at == 0) line(TFT_WHITE, String(tag) + idle);
    else line(st.err.length() ? TFT_RED : TFT_GREEN,
              String(tag) + (st.err.length() ? st.err : String(st.code)) + " in " +
              String(st.ms / 1000.0, 1) + "s, " + ago(millis() - st.at) + " ago");
  };
  srcLine("NS    ", useNs(), nsStat, "no fetch yet");
  srcLine("Dex   ", useDexcom(), dexStat, useNs() ? "backup, not needed yet" : "no fetch yet");
  line(TFT_WHITE, "OK/fail  NS " + String(nsStat.ok) + "/" + String(nsStat.fail) +
                  "   Dex " + String(dexStat.ok) + "/" + String(dexStat.fail));
  long age = ageMinutes();
  line(TFT_WHITE, "Reading  " + (lastSgv < 0 ? String("--") : String(lastSgv) + " " + lastDir) +
                  (age < 0 ? String("") : ", " + String(age) + "m old") +
                  (lastSrc[0] ? String(", ") + lastSrc : String("")));
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

// Short press: debug page on/off. Hold 2 s and release: Bluetooth log on/off.
// Keep holding to 6 s: setup page (WiFi + data source).
void enterSetup(const char* why);
bool btnDown = false, btnHint = false;
uint32_t btnT0 = 0;
void drawHoldHint() {                              // shown once the press counts as "long"
  int h = 20, y = tft.height() - h;
  tft.fillRect(0, y, tft.width(), h, TFT_BLACK);
  tft.setTextFont(2); tft.setTextSize(1); tft.setTextDatum(BC_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("Release: Bluetooth.  Keep holding: setup", tft.width() / 2, tft.height() - 2);
}
void pollButton() {
  bool down = digitalRead(BOARD_BUTTON) == LOW;
  uint32_t now = millis();
  if (down && !btnDown) { btnDown = true; btnHint = false; btnT0 = now; }
  else if (down) {
    if (!btnHint && now - btnT0 >= LONG_PRESS_MS) { btnHint = true; drawHoldHint(); }
    if (now - btnT0 >= SETUP_PRESS_MS) enterSetup("button");   // doesn't return
  } else if (btnDown) {
    btnDown = false;
    uint32_t held = now - btnT0;
    if (held >= LONG_PRESS_MS) { bleOn ? bleStop() : bleStart(); showDebug(debugPage); }
    else if (held >= 30) showDebug(!debugPage);    // 30 ms debounce
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
  logf("settings: %s", loadConfig() ? "from setup page" : "defaults (nothing saved yet)");
  if (!haveWifi() || (!useNs() && !useDexcom())) enterSetup("not set up yet");   // first boot
  if (useNs() && useDexcom()) logf("source: Nightscout, Dexcom (%s) as backup", cfg.dexRegion.c_str());
  else if (useDexcom())       logf("source: Dexcom Share, region %s", cfg.dexRegion.c_str());
  else                        logf("source: Nightscout");
  addNetworks();
  connectWiFi();
  configTzTime(cfg.tz.c_str(), "pool.ntp.org", "time.google.com");  // age uses the UTC epoch; TZ only affects the clock
  if (useDexcom())                                 // Dexcom's certificate check needs the date
    for (uint32_t t0 = millis(); time(nullptr) < 1000000000L && millis() - t0 < 10000; ) delay(100);
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
    logf("time: synced, %s (%s)", b, cfg.tz.c_str());
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
