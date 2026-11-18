// Copy to secrets.h (gitignored) and fill in.
#pragma once
// Every WiFi network the display may use, one {"name", "password"} per line.
// It joins whichever is in range (strongest first). Add or remove lines freely.
// iPhone hotspot: Settings > Personal Hotspot > turn on "Maximize Compatibility" (ESP32 is 2.4 GHz only).
#define SECRET_WIFI_NETWORKS \
  {"YOUR_HOME_WIFI",    "YOUR_HOME_PASSWORD"}, \
  {"YOUR_HOTSPOT_NAME", "YOUR_HOTSPOT_PASSWORD"}
#define SECRET_NS_HOST   "yoursite.example.com"   // your Nightscout host, no https://
#define SECRET_NS_TOKEN  ""   // optional: leave empty if your site is readable without one
// #define SECRET_TZ "PST8PDT,M3.2.0,M11.1.0"   // optional clock time zone (default JST-9)
