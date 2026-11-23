// OPTIONAL. Normally you set everything on the board's setup page (hold the button 6 s).
// To compile defaults in instead: copy to secrets.h (gitignored) and fill in.
// They are used until something is saved on the setup page.
#pragma once
// Every WiFi network the display may use, one {"name", "password"} per line.
// It joins whichever is in range (strongest first). Add or remove lines freely.
// iPhone hotspot: Settings > Personal Hotspot > turn on "Maximize Compatibility" (ESP32 is 2.4 GHz only).
#define SECRET_WIFI_NETWORKS \
  {"YOUR_HOME_WIFI",    "YOUR_HOME_PASSWORD"}, \
  {"YOUR_HOTSPOT_NAME", "YOUR_HOTSPOT_PASSWORD"}
// --- Data source: Nightscout, Dexcom, or both (Nightscout first, Dexcom as backup) ---
// Nightscout:
#define SECRET_NS_HOST   "yoursite.example.com"   // your Nightscout host, no https://
#define SECRET_NS_TOKEN  ""   // optional: leave empty if your site is readable without one
// Dexcom Share (no Nightscout needed). Uncomment to use.
// Use the sensor WEARER's Dexcom login (not a follower's), with Share turned on in the Dexcom app.
// #define SECRET_DEXCOM_USER   "you@example.com"
// #define SECRET_DEXCOM_PASS   "your-dexcom-password"
// #define SECRET_DEXCOM_REGION "jp"   // "us", "ous" (outside the US) or "jp" (Japan)
// #define SECRET_TZ "PST8PDT,M3.2.0,M11.1.0"   // optional clock time zone (default JST-9)
// #define SECRET_MMOL                           // optional: show mmol/L instead of mg/dL
