// Improv Wi-Fi over USB serial (https://www.improv-wifi.com/serial/). Right after installing
// from glucoled.com, the install page offers "Connect to Wi-Fi": the WiFi name and password are
// typed on the computer and sent down the USB cable. Once the board is on the home network the
// page shows "Visit device", which opens the setup page over that network, so the rest (Dexcom,
// time zone...) is filled in on the computer too, which still has internet.
//
// The setup page is only reachable on the home network while the setup screen is showing.

const uint8_t IMP_READY = 2, IMP_PROVISIONING = 3, IMP_PROVISIONED = 4;          // states
const uint8_t IMP_ERR_BAD_PACKET = 1, IMP_ERR_UNKNOWN_CMD = 2, IMP_ERR_CONNECT = 3; // errors
const uint8_t IMP_TYPE_STATE = 1, IMP_TYPE_ERROR = 2, IMP_TYPE_RPC = 3, IMP_TYPE_RESULT = 4;
const uint32_t IMP_JOIN_MS = 20000;

bool   improvInSetup = false;                      // set by enterSetup()
bool   improvJoined  = false;                      // setup mode: joined a home network via Improv
String improvUrl;                                  // setup page on the home network, once joined

const int IMP_MAX_SCAN = 20;                       // setup mode answers scans from enterSetup's scan
String impScanSsid[IMP_MAX_SCAN];
int    impScanRssi[IMP_MAX_SCAN];
bool   impScanLocked[IMP_MAX_SCAN];
int    impScanN = 0;

uint8_t impBuf[9 + 255 + 1];
int     impLen = 0;

#if CONFIG_IDF_TARGET_ESP32S3
const char* IMP_CHIP = "ESP32-S3";
#else
const char* IMP_CHIP = "ESP32";
#endif

void impSend(uint8_t type, const uint8_t* data, uint8_t len) {
  uint8_t hdr[9] = {'I', 'M', 'P', 'R', 'O', 'V', 1, type, len};
  uint8_t sum = 0;
  for (uint8_t b : hdr) sum += b;
  for (int i = 0; i < len; i++) sum += data[i];
  Serial.write('\n');                              // the page only looks for packets at a line start
  Serial.write(hdr, sizeof hdr);
  if (len) Serial.write(data, len);
  Serial.write(sum);
  Serial.write('\n');
}

void impState(uint8_t s) { impSend(IMP_TYPE_STATE, &s, 1); }
void impError(uint8_t e) { impSend(IMP_TYPE_ERROR, &e, 1); }

void impResult(uint8_t cmd, const String* strs, int n) {
  uint8_t d[255];
  int len = 2;
  for (int i = 0; i < n; i++) {
    int sl = min((int)strs[i].length(), 250 - len);
    if (sl < 0) break;
    d[len++] = sl;
    memcpy(d + len, strs[i].c_str(), sl);
    len += sl;
  }
  d[0] = cmd; d[1] = len - 2;
  impSend(IMP_TYPE_RESULT, d, len);
}

uint8_t improvState() {
  if (improvInSetup) return improvJoined ? IMP_PROVISIONED : IMP_READY;
  return WiFi.status() == WL_CONNECTED ? IMP_PROVISIONED : IMP_READY;
}

void impSendUrl(uint8_t cmd) {                     // the result of a provisioned state / join
  if (improvInSetup && improvUrl.length()) impResult(cmd, &improvUrl, 1);
  else impResult(cmd, nullptr, 0);                 // running normally: no web page to visit
}

void improvRemember(const String& ssid, const String& pass) {   // put it first in the saved list
  String s[MAX_NETS], p[MAX_NETS];
  int n = 0;
  s[n] = ssid; p[n] = pass; n++;
  for (int i = 0; i < MAX_NETS && n < MAX_NETS; i++)
    if (usable(cfg.ssid[i]) && cfg.ssid[i] != ssid) { s[n] = cfg.ssid[i]; p[n] = cfg.pass[i]; n++; }
  for (int i = 0; i < MAX_NETS; i++) { cfg.ssid[i] = s[i]; cfg.pass[i] = p[i]; }
  saveConfig();
}

bool improvJoin(const String& ssid, const String& pass) {
  logf("improv: joining \"%s\"", ssid.c_str());
  if (improvInSetup) drawSetupMessage("Joining WiFi", ssid.c_str());
  else WiFi.disconnect();
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < IMP_JOIN_MS) delay(100);
  if (WiFi.status() != WL_CONNECTED) {
    logf("improv: could not join \"%s\" (status %d)", ssid.c_str(), WiFi.status());
    WiFi.disconnect();
    if (improvInSetup) drawSetupScreen();
    else connectWiFi();                            // back to the networks it already knew
    return false;
  }
  improvRemember(ssid, pass);
  logf("improv: joined \"%s\", ip %s; network saved", ssid.c_str(), WiFi.localIP().toString().c_str());
  if (improvInSetup) {
    improvJoined = true;
    improvUrl = "http://" + WiFi.localIP().toString() + "/";
    drawSetupLan();
  } else {
    wifiMulti.addAP(ssid.c_str(), pass.c_str());
  }
  return true;
}

void improvScanReply() {
  if (!improvInSetup) {                            // running normally: scan now
    improvKeepScan(WiFi.scanNetworks());
    WiFi.scanDelete();
  }
  for (int i = 0; i < impScanN; i++) {
    String r[3] = {impScanSsid[i], String(impScanRssi[i]), impScanLocked[i] ? "YES" : "NO"};
    impResult(4, r, 3);
  }
  impResult(4, nullptr, 0);                        // empty result = end of list
}

void improvRpc(const uint8_t* d, int len) {
  if (len < 2 || d[1] > len - 2) { impError(IMP_ERR_BAD_PACKET); return; }
  uint8_t cmd = d[0];
  const uint8_t* a = d + 2;
  int alen = d[1];
  switch (cmd) {
    case 1: {                                      // WiFi settings: [len ssid][len password]
      int sl = alen > 0 ? a[0] : -1;
      int pl = (sl >= 0 && 1 + sl < alen) ? a[1 + sl] : -1;
      if (sl <= 0 || pl < 0 || 2 + sl + pl > alen) { impError(IMP_ERR_BAD_PACKET); return; }
      String ssid, pass;
      for (int i = 0; i < sl; i++) ssid += (char)a[1 + i];
      for (int i = 0; i < pl; i++) pass += (char)a[2 + sl + i];
      impState(IMP_PROVISIONING);
      if (improvJoin(ssid, pass)) { impState(IMP_PROVISIONED); impSendUrl(1); }
      else { impError(IMP_ERR_CONNECT); impState(IMP_READY); }
      break;
    }
    case 2: {                                      // current state (+ URL if provisioned)
      uint8_t s = improvState();
      impState(s);
      if (s == IMP_PROVISIONED) impSendUrl(2);
      break;
    }
    case 3: {                                      // device info
      String r[4] = {"Glucoled", FW_VERSION, IMP_CHIP, HOSTNAME};
      impResult(3, r, 4);
      break;
    }
    case 4: improvScanReply(); break;
    default: impError(IMP_ERR_UNKNOWN_CMD);
  }
}

// Called from loop() and the setup screen's loop. Reads whatever has arrived; anything
// that isn't an Improv packet is ignored.
void improvPoll() {
  static const char HDR[] = "IMPROV";
  while (Serial.available()) {
    uint8_t b = Serial.read();
    if (impLen < 6) {                              // matching the "IMPROV" header
      if (b == (uint8_t)HDR[impLen]) impBuf[impLen++] = b;
      else impLen = (b == 'I') ? (impBuf[0] = b, 1) : 0;
      continue;
    }
    impBuf[impLen++] = b;
    if (impLen == 7 && b != 1) { impLen = 0; continue; }   // protocol version 1 only
    if (impLen < 9) continue;
    int total = 9 + impBuf[8] + 1;
    if (impLen < total) continue;
    uint8_t sum = 0;
    for (int i = 0; i < total - 1; i++) sum += impBuf[i];
    uint8_t type = impBuf[7];
    impLen = 0;
    if (sum != impBuf[total - 1]) { impError(IMP_ERR_BAD_PACKET); continue; }
    if (type == IMP_TYPE_RPC) improvRpc(impBuf + 9, total - 10);
  }
}

// enterSetup() scans before starting its own network; keep that list for the install page.
void improvKeepScan(int found) {
  impScanN = 0;
  for (int i = 0; i < found && impScanN < IMP_MAX_SCAN; i++) {
    if (!WiFi.SSID(i).length()) continue;
    impScanSsid[impScanN] = WiFi.SSID(i); impScanRssi[impScanN] = WiFi.RSSI(i);
    impScanLocked[impScanN] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN; impScanN++;
  }
}
