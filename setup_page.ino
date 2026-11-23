// Setup page: the board makes its own WiFi network, the phone joins it (scan the QR code on
// the screen), and a form opens for WiFi + Nightscout/Dexcom + time zone. Saving stores it on
// the board and restarts. Runs only in setup mode: no web server is left running normally.
//
// Setup mode starts on first boot (nothing set) or when the button is held 6 s.

// (WebServer, DNSServer and qrcode.h are included at the top of glucoled.ino.)

const char* SETUP_SSID = "Glucoled-Setup";
const uint32_t SETUP_TIMEOUT_MS = 15UL * 60 * 1000;   // give up and restart (if already set up)

WebServer setupWeb(80);
DNSServer setupDns;
String setupPass;                                  // random 8 digits per session, shown on screen
String scanList;                                   // <option>s of nearby networks
bool setupDone = false;

// ---- screen ----
int qrX, qrY, qrScale;
void drawQrModules(esp_qrcode_handle_t q) {
  int n = esp_qrcode_get_size(q);
  int box = (n + 4) * qrScale;                     // 2-module white border (QR "quiet zone")
  tft.fillRect(qrX, qrY, box, box, TFT_WHITE);
  for (int y = 0; y < n; y++)
    for (int x = 0; x < n; x++)
      if (esp_qrcode_get_module(q, x, y))
        tft.fillRect(qrX + (x + 2) * qrScale, qrY + (y + 2) * qrScale, qrScale, qrScale, TFT_BLACK);
}

void drawSetupScreen() {
  tft.fillScreen(TFT_BLACK);
  // "WIFI:" QR codes join the network straight from the phone camera (iPhone and Android).
  String join = String("WIFI:T:WPA;S:") + SETUP_SSID + ";P:" + setupPass + ";;";
  qrScale = 4; qrX = 4; qrY = (tft.height() - 33 * qrScale) / 2;   // fits up to version 4 (33 modules)
  esp_qrcode_config_t qc = ESP_QRCODE_CONFIG_DEFAULT();
  qc.display_func = drawQrModules;
  qc.max_qrcode_version = 4;
  qc.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
  esp_qrcode_generate(&qc, join.c_str());

  int x = qrX + 33 * qrScale + 10, y = 6, lh = 17;
  tft.setTextFont(2); tft.setTextSize(1); tft.setTextDatum(TL_DATUM);
  auto line = [&](uint16_t c, const String& t) { tft.setTextColor(c, TFT_BLACK); tft.drawString(t, x, y); y += lh; };
  line(TFT_YELLOW, "Glucoled setup");
  line(TFT_WHITE, "1. Scan with camera");
  line(TFT_DARKGREY, "or join WiFi:");
  line(TFT_CYAN, SETUP_SSID);
  line(TFT_DARKGREY, "password:");
  line(TFT_CYAN, setupPass);
  line(TFT_WHITE, "2. Fill in the page");
  line(TFT_DARKGREY, "(or open 192.168.4.1)");
  line(TFT_DARKGREY, "Press button to cancel");
}

void drawSetupMessage(const char* a, const char* b) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(4); tft.setTextDatum(MC_DATUM); tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString(a, tft.width() / 2, tft.height() / 2 - 14);
  tft.setTextFont(2); tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(b, tft.width() / 2, tft.height() / 2 + 16);
}

// ---- page ----
String esc(const String& s) {                      // HTML-escape values put back into the form
  String o;
  for (char c : s) {
    if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;"; else if (c == '\'') o += "&#39;"; else o += c;
  }
  return o;
}

static const char PAGE_HEAD[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Glucoled setup</title><style>
body{font:16px -apple-system,system-ui,sans-serif;margin:0 auto;padding:16px;max-width:520px;background:#111;color:#eee}
h1{font-size:22px;margin:0 0 4px}h2{font-size:17px;margin:22px 0 6px;color:#7c7}
p.s{color:#999;font-size:13px;margin:4px 0 10px}
label{display:block;font-size:13px;color:#bbb;margin:10px 0 3px}
input,select{width:100%;box-sizing:border-box;font-size:16px;padding:9px;border-radius:8px;border:1px solid #444;background:#222;color:#eee}
.row{display:flex;gap:8px}.row>div{flex:1}
button{width:100%;margin-top:22px;padding:13px;font-size:17px;border:0;border-radius:10px;background:#3a3;color:#fff}
button.x{background:#333;color:#c66;font-size:14px;padding:9px;margin-top:30px}
.e{background:#511;padding:10px;border-radius:8px}
</style></head><body>)HTML";

static const char* TZS[][2] = {
  {"JST-9", "Japan / 日本"}, {"KST-9", "Korea"}, {"CST-8", "China, Taiwan, Hong Kong, Singapore (UTC+8)"},
  {"AEST-10AEDT,M10.1.0,M4.1.0/3", "Australia: Sydney, Melbourne"},
  {"NZST-12NZDT,M9.5.0,M4.1.0/3", "New Zealand"},
  {"GMT0BST,M3.5.0/1,M10.5.0", "UK, Ireland"}, {"CET-1CEST,M3.5.0,M10.5.0/3", "Central Europe"},
  {"EST5EDT,M3.2.0,M11.1.0", "US Eastern"}, {"CST6CDT,M3.2.0,M11.1.0", "US Central"},
  {"MST7MDT,M3.2.0,M11.1.0", "US Mountain"}, {"MST7", "US Arizona"},
  {"PST8PDT,M3.2.0,M11.1.0", "US Pacific"}, {"HST10", "Hawaii"},
};

void sendForm(const String& error) {
  String h = FPSTR(PAGE_HEAD);
  h += "<h1>Glucoled setup</h1><p class=s>Glucoled " + String(FW_VERSION) +
       ". Settings are saved on this board only.</p>";
  if (error.length()) h += "<p class=e>" + esc(error) + "</p>";
  h += "<form method=post action=/save>";

  h += "<h2>WiFi</h2><p class=s>2.4 GHz only. Add home and your phone's hotspot; it joins whichever is "
       "in range. iPhone hotspot: turn on <i>Maximize Compatibility</i>.<br>"
       "パスワード欄は空欄のままなら変更されません。</p>";
  h += "<datalist id=nets>" + scanList + "</datalist>";
  for (int i = 0; i < MAX_NETS; i++) {
    h += "<div class=row><div><label>WiFi name " + String(i + 1) + "</label><input name=s" + i +
         " list=nets autocapitalize=off autocorrect=off value=\"" + esc(cfg.ssid[i]) + "\"></div>";
    h += "<div><label>Password</label><input name=p" + String(i) + " type=password placeholder=\"" +
         (cfg.pass[i].length() ? "(unchanged)" : "") + "\"></div></div>";
  }

  h += "<h2>Dexcom</h2><p class=s>The sensor <b>wearer's</b> Dexcom login (not a follower's), with "
       "Share turned on in the Dexcom app. 装着者本人のDexcomアカウント。</p>";
  h += "<label>Dexcom username / email</label><input name=du autocapitalize=off autocorrect=off value=\"" +
       esc(cfg.dexUser) + "\">";
  h += "<label>Dexcom password</label><input name=dp type=password placeholder=\"" +
       String(cfg.dexPass.length() ? "(unchanged)" : "") + "\">";
  // No default region: a wrong one looks like a wrong password, so make people pick.
  bool regionSet = cfg.dexUser.length() > 0;
  h += "<label>Account region / 地域</label><select name=dr>";
  h += String("<option value=\"\"") + (regionSet ? "" : " selected") + ">-- choose / 選択 --</option>";
  const char* regs[][2] = {{"jp", "Japan / 日本"}, {"us", "United States"}, {"ous", "Outside the US (Europe etc.)"}};
  for (auto& r : regs)
    h += String("<option value=") + r[0] + (regionSet && cfg.dexRegion == r[0] ? " selected" : "") + ">" + r[1] + "</option>";
  h += "</select>";

  h += "<h2>Nightscout (optional)</h2><p class=s>If you also run Nightscout, it's asked first and "
       "Dexcom becomes the backup. Leave empty to use Dexcom only.</p>";
  h += "<label>Nightscout address</label><input name=nh autocapitalize=off autocorrect=off "
       "placeholder=\"yoursite.example.com\" value=\"" + esc(cfg.nsHost) + "\">";
  h += "<label>Token (only if your site needs one; never the API secret)</label><input name=nt "
       "type=password autocapitalize=off placeholder=\"" + String(cfg.nsToken.length() ? "(unchanged)" : "") + "\">";

  h += "<h2>Clock</h2><label>Time zone / タイムゾーン</label><select name=tz>";
  bool known = false;
  for (auto& z : TZS) {
    bool sel = cfg.tz == z[0]; known |= sel;
    h += String("<option value=\"") + z[0] + "\"" + (sel ? " selected" : "") + ">" + z[1] + "</option>";
  }
  h += String("<option value=custom") + (known ? "" : " selected") + ">Other (POSIX TZ below)</option></select>";
  h += "<label>Other time zone (POSIX, e.g. EST5EDT,M3.2.0,M11.1.0)</label><input name=tzc autocapitalize=off "
       "value=\"" + (known ? String("") : esc(cfg.tz)) + "\">";

  h += String("<h2>Units / 単位</h2><select name=un><option value=mgdl") + (cfg.mmol ? "" : " selected") +
       ">mg/dL</option><option value=mmol" + (cfg.mmol ? " selected" : "") + ">mmol/L</option></select>";

  h += "<button>Save and restart / 保存して再起動</button></form>";
  h += "<form method=post action=/erase onsubmit=\"return confirm('Erase all settings?')\">"
       "<button class=x>Erase all settings / 設定を消去</button></form></body></html>";
  setupWeb.send(200, "text/html; charset=utf-8", h);
}

String cleanHost(String h) {                       // people paste full URLs: keep just the host
  h.trim();
  if (h.startsWith("https://")) h = h.substring(8); else if (h.startsWith("http://")) h = h.substring(7);
  int cut = h.indexOf('/'); if (cut >= 0) h = h.substring(0, cut);
  cut = h.indexOf('?'); if (cut >= 0) h = h.substring(0, cut);
  return h;
}

void handleSave() {
  Config c = cfg;                                  // start from current: blank passwords keep the old one
  int n = 0;
  Config old = cfg;
  for (int i = 0; i < MAX_NETS; i++) { c.ssid[i] = ""; c.pass[i] = ""; }
  for (int i = 0; i < MAX_NETS; i++) {
    String s = setupWeb.arg("s" + String(i));      // SSIDs may have spaces at the ends; keep them
    String p = setupWeb.arg("p" + String(i));
    if (!s.length()) continue;
    if (!p.length())                               // unchanged: find this network's saved password
      for (int j = 0; j < MAX_NETS; j++) if (old.ssid[j] == s) { p = old.pass[j]; break; }
    c.ssid[n] = s; c.pass[n] = p; n++;
  }
  String du = setupWeb.arg("du"); du.trim();
  if (du != old.dexUser) c.dexPass = "";           // new account: don't reuse the old password
  c.dexUser = du;
  if (setupWeb.arg("dp").length()) c.dexPass = setupWeb.arg("dp");
  c.dexRegion = setupWeb.arg("dr");
  c.nsHost = cleanHost(setupWeb.arg("nh"));
  if (!c.nsHost.length()) c.nsToken = "";
  if (setupWeb.arg("nt").length()) { c.nsToken = setupWeb.arg("nt"); c.nsToken.trim(); }
  String tz = setupWeb.arg("tz");
  if (tz == "custom") { tz = setupWeb.arg("tzc"); tz.trim(); }
  c.tz = tz.length() ? tz : "JST-9";
  c.mmol = setupWeb.arg("un") == "mmol";

  if (n == 0) { cfg = c; return sendForm("Add at least one WiFi network."); }
  bool dex = c.dexUser.length() && c.dexPass.length(), ns = c.nsHost.length();
  if (c.dexUser.length() && !c.dexPass.length()) { cfg = c; return sendForm("Enter the Dexcom password."); }
  if (c.dexUser.length() && !c.dexRegion.length()) { cfg = c; return sendForm("Choose the Dexcom account region (Japan for Japanese accounts). / Dexcomの地域を選んでください。"); }
  if (!c.dexRegion.length()) c.dexRegion = "us";
  if (!dex && !ns) { cfg = c; return sendForm("Fill in Dexcom or Nightscout (or both)."); }

  cfg = c;
  saveConfig();
  logf("setup: saved %d WiFi network(s), %s%s%s", n, dex ? "Dexcom " : "", dex ? c.dexRegion.c_str() : "",
       ns ? (dex ? " + Nightscout" : "Nightscout") : "");
  setupWeb.send(200, "text/html; charset=utf-8", String(FPSTR(PAGE_HEAD)) +
    "<h1>Saved ✓</h1><p>Glucoled is restarting and will join your WiFi. You can close this page "
    "and reconnect your phone to your normal WiFi.</p><p>保存しました。再起動します。</p></body></html>");
  setupDone = true;
}

void handleErase() {
  eraseConfig();
  logf("setup: settings erased");
  setupWeb.send(200, "text/html; charset=utf-8", String(FPSTR(PAGE_HEAD)) +
    "<h1>Erased</h1><p>All settings were removed. Glucoled restarts into setup.</p></body></html>");
  setupDone = true;
}

// Phones probe a known URL to detect a login page (captive portal). Answering every
// request with a redirect to the form makes the setup page pop up by itself.
void handleAny() {
  setupWeb.sendHeader("Location", "http://192.168.4.1/", true);
  setupWeb.send(302, "text/plain", "");
}

void enterSetup(const char* why) {
  logf("setup: starting (%s)", why);
  if (bleOn) bleStop();
  bool wasSetUp = haveWifi() && (useNs() || useDexcom());

  char pw[9];
  snprintf(pw, sizeof pw, "%08lu", (unsigned long)(esp_random() % 100000000UL));
  setupPass = pw;

  drawSetupMessage("Setup", "Looking for WiFi networks...");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  int found = WiFi.scanNetworks();
  scanList = "";
  for (int i = 0; i < found && i < 25; i++)
    if (WiFi.SSID(i).length() && scanList.indexOf("\"" + esc(WiFi.SSID(i)) + "\"") < 0)
      scanList += "<option value=\"" + esc(WiFi.SSID(i)) + "\">";
  WiFi.scanDelete();

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(SETUP_SSID, setupPass.c_str());
  setupDns.start(53, "*", IPAddress(192, 168, 4, 1));
  setupWeb.on("/", HTTP_GET, []() { sendForm(""); });
  setupWeb.on("/save", HTTP_POST, handleSave);
  setupWeb.on("/erase", HTTP_POST, handleErase);
  setupWeb.onNotFound(handleAny);
  setupWeb.begin();
  drawSetupScreen();
  logf("setup: WiFi \"%s\" up, page at 192.168.4.1", SETUP_SSID);

  uint32_t t0 = millis(), doneAt = 0;
  bool wasDown = digitalRead(BOARD_BUTTON) == LOW;   // still held from the 6 s press
  while (true) {
    setupDns.processNextRequest();
    setupWeb.handleClient();
    if (setupDone && !doneAt) { doneAt = millis(); drawSetupMessage("Saved", "Restarting..."); }
    if (doneAt && millis() - doneAt > 1500) break;   // let the "saved" page reach the phone
    bool down = digitalRead(BOARD_BUTTON) == LOW;
    if (down && !wasDown) { logf("setup: cancelled"); drawSetupMessage("Cancelled", "Restarting..."); delay(800); break; }
    wasDown = down;
    if (wasSetUp && millis() - t0 > SETUP_TIMEOUT_MS) { logf("setup: timed out"); break; }
    delay(2);
  }
  ESP.restart();
}
