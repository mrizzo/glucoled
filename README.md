# Glucoled

English | [日本語](README.ja.md)

**For diabetics who just want to see their blood sugar at a glance.**

An affordable HUD for your blood sugar: on the bike, in the car, at your desk, by your bed.
No unlocking your phone, no turning your wrist.

A tiny desk/car display for your glucose, from [Nightscout](https://nightscout.github.io/) or
straight from **Dexcom Share** (including Japanese Dexcom accounts):
**just the number and the trend arrow**. No alarms, no graphs, no app.

Open-source software on an off-the-shelf board. No subscription, no soldering, no programming.

**Built by someone who wears the sensor.** No alarms, no followers, no "are you ok?" texts.
Just your number, when you want to look.

<p align="center">
  <img src="docs/ideaspark.jpg" alt="Glucoled on the ideaspark ESP32 1.9-inch board on a desk, showing 19:04, NS and a green 93 with a flat arrow" width="49%">
  <img src="docs/tdisplay-s3.jpg" alt="Glucoled on a LilyGO T-Display S3 mounted on a motorcycle dash at night, showing a green 96 with a flat arrow" width="49%">
</p>

<p align="center"><b>Temperature. Oil pressure. Fuel. Speed. Blood sugar.</b><br>Just another gauge on the dash.</p>

- Number and arrow are **coloured by range**, like Loop Follow's time-in-range chart:
  red under 54, darker red 54–69, green 70–180, yellow 181–250, orange over 250
  (change the cut-offs with `RANGE_VERY_LOW`, `RANGE_LOW`, `RANGE_HIGH`, `RANGE_VERY_HIGH`
  in `secrets.h`).
- **Grey** when the latest reading is older than 11 minutes (or the board doesn't know the
  time yet), so a stale value never looks current.
- **mg/dL or mmol/L**, chosen on the setup page (colours use the same ranges either way).
- Waits up to **15 seconds** for Nightscout to answer, so slow hosted sites (some take ~5 s
  on a cold request) don't make it give up.
- Shows the **time** (HH:MM) above the number, set from the internet, so it doubles as a desk clock.
- Remembers **several WiFi networks**, e.g. home + your phone's hotspot for the car.
- Total cost is the board: about ¥2,000 / US$15.

## Install

**No tools needed:** open **[glucoled.com](https://glucoled.com)** in Chrome or Edge,
plug in the board and click **Install**, then set it up from your phone. The page walks through every click,
in English and Japanese.
Setup videos: [youtube.com/@GlucoLED](https://www.youtube.com/@GlucoLED).
（日本語の説明は [README.ja.md](README.ja.md)。）

The rest of this README is for building it yourself.

> **Not a medical device.** Glucoled is a convenience display. Don't make treatment
> decisions from it. Use your CGM app / receiver and its alarms.

## Boards

Both boards are tested and run the same code. Pick one:

| Board | Where | Notes |
|---|---|---|
| **ideaspark ESP32 1.9" ST7789 170×320** (ESP32-WROOM-32, CH340 USB-C) | Amazon JP, ~¥2,000 | Default. Power from USB or a USB power bank. 3D-printable case: [case/ideaspark](case/ideaspark/). |
| **LilyGO T-Display S3** (ESP32-S3, 1.9" 170×320) | [LilyGO store](https://lilygo.cc/products/t-display-s3?variant=42585826558133), ~US$15 | Has a battery connector with charging and two buttons. |

Use a USB-C **data** cable. Charge-only cables won't show the board.
No USB driver is needed on recent macOS.

## Setup

### 1. Install the tools (once)

[arduino-cli](https://arduino.github.io/arduino-cli/), plus the ESP32 core and two libraries:

```sh
brew install arduino-cli          # or see the arduino-cli docs for Windows/Linux
arduino-cli config init
arduino-cli config add board_manager.additional_urls \
  https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.2.0
arduino-cli lib install "TFT_eSPI@2.5.43" "ArduinoJson@7.4.3"
```

On Apple Silicon Macs, the Arduino toolchain still needs Rosetta for one helper (ctags):
`softwareupdate --install-rosetta --agree-to-license`.

You do **not** need to edit the TFT_eSPI library's `User_Setup.h`. Each board's pins live
in `board_*.h` here, and `build.sh` passes the right one in.

### 2. Build and flash

Plug in **one** board, then:

```sh
./build.sh upload                    # ideaspark (default)
BOARD=tdisplays3 ./build.sh upload   # LilyGO T-Display S3
```

`./build.sh` without `upload` only compiles. The port is auto-detected; override it with
`PORT=/dev/cu.xxx`.

### 3. Set it up from your phone

On first start the screen shows **Glucoled setup** with a QR code.

<p align="center">
  <img src="docs/setup-screen.jpg" alt="Glucoled setup screen on a T-Display S3: a QR code, the Glucoled-Setup network name, a one-time password and the steps" width="58%">
  <img src="docs/setup-form.jpg" alt="The setup page on an iPhone: WiFi names and passwords, Dexcom username, password and region set to Japan" width="30%">
</p>

1. Scan the QR code with your phone's camera and tap to join the **Glucoled-Setup** WiFi
   (or join it by hand with the password shown on the screen).
2. The setup page opens by itself. If it doesn't, open `http://192.168.4.1`.
3. Fill in:
   - **WiFi:** up to four networks, e.g. home and your phone's hotspot. Glucoled joins whichever
     is in range.
   - **Dexcom:** the sensor wearer's login and the account's region, and/or
   - **Nightscout:** your site's address (and a read token if it needs one).
   - **Time zone** for the clock.
   - **Units**: mg/dL or mmol/L.
   - **Clock**: 24-hour (15:42) or 12-hour (3:42 PM).
4. Tap **Save and restart**. Your number appears within about 20 seconds and refreshes every
   60 seconds.

If the board can't join WiFi (or has no data source), it says so at the bottom of the screen:

<p align="center">
  <img src="docs/nowifi.jpg" alt="Glucoled showing -- with the hint: No WiFi. Hold the button 6 s for setup" width="50%">
</p>

To change settings later, **hold the button for 6 seconds** (the screen says "Keep holding: setup"
after 2). Press it again on the setup screen to cancel. The page also has **Erase all settings**,
e.g. before giving the board to someone else.

Settings are stored on the board only. The setup network and page exist only while the setup
screen is showing; normally no web server runs.

Notes:

- **WiFi:** the ESP32 only does 2.4 GHz. For an **iPhone hotspot**, turn on
  *Settings → Personal Hotspot → Maximize Compatibility*, or the board won't see it.
- **Nightscout token:** leave it empty if your site can be read without logging in (the
  default `AUTH_DEFAULT_ROLES=readable`). Otherwise create a token with the `readable` role
  under *Admin Tools → Subjects*. Never use your API_SECRET.
- **Sources:** set Nightscout, Dexcom, or both. With both, Nightscout is asked first (it's
  usually fastest when an uploader like Trio/Loop/xDrip is running). If its reading is missing
  or 6+ minutes old (the uploader missed one), Dexcom is asked too and the newer reading is
  shown. A small **NS** or **D** in the top-right corner shows where the number came from.
- **Dexcom:**
  - Use the **sensor wearer's** Dexcom login, the one the Dexcom app is signed into, not a
    follower's. **Share must be on** in the Dexcom app.
  - Pick the region the account lives in. Japanese accounts only work with **Japan** (many other
    displays only know the US and international servers, so they reject Japanese logins).
  - Your Dexcom **password is stored on the board**. Anyone with the board and a USB cable could
    read it out, so treat the board like a key (or use *Erase all settings* before passing it on).
  - This uses Dexcom's unofficial Share API (the one the Follow app uses, as in
    [pydexcom](https://github.com/gagebenne/pydexcom)). Dexcom could change it at any time.
  - If Dexcom rejects the login, Glucoled stops trying until you fix it on the setup page, so a
    wrong password can't lock your account.

#### Optional: built-in defaults (developers)

If you flash several boards for yourself, you can compile settings in instead of using the
setup page: `cp secrets.example.h secrets.h` (gitignored) and fill it in. They're used until
something is saved on the setup page.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `board not found` | Use a data cable, or try another port/hub. |
| "No WiFi. Hold the button 6 s for setup" | Check the WiFi name/password on the setup page (exact spelling), and that it's 2.4 GHz. |
| The setup page doesn't pop up | Open `http://192.168.4.1` in the phone's browser while joined to Glucoled-Setup. Turn off mobile data if the phone keeps leaving the network. |
| `--` instead of a number | WiFi works, but the data source didn't answer. Open the debug page (below) for the reason. Nightscout: check the address and token; "certificate not trusted" means the site uses a self-signed certificate, so give it a real one (e.g. Let's Encrypt). Dexcom: check the login, region and that Share is on. |
| Number is always grey | No new reading for 11+ minutes. Check Nightscout itself, or that the Dexcom app is running and sharing. |
| Colours inverted (ideaspark) | In `board_ideaspark_19.h`, swap `TFT_INVERSION_ON` for `TFT_INVERSION_OFF`. |

## Debugging

Three ways to see what it's doing. None of them open a network port.

- **USB log:** plug it into a computer and run
  `arduino-cli monitor -p <port> -c baudrate=115200`. It shows WiFi joins and drops, each Nightscout
  fetch (HTTP code, how long it took, the reading) and why a fetch failed.
- **Debug page:** a **short press** of the BOOT button (GPIO0; on the T-Display S3, the lower orange button with the screen landscape and USB on the right) switches to a status screen: WiFi network, signal and
  IP, the last Nightscout result and timing, fetch counts, uptime and free memory. Press again,
  or wait 2 minutes, to go back.
- **Bluetooth log:** **hold** the BOOT button for 2 seconds, then release. A blue `BT` appears top-left, and for
  10 minutes the board advertises as "Glucoled". In *Bluefruit Connect* (iOS/Android), tap
  Glucoled, then **UART**. On Android, *Serial Bluetooth Terminal* also works. You get the last 40
  log lines and then live ones. (Generic BLE apps like *nRF Connect* show only raw packets, and
  their "parsers" decode the text as nonsense heart-rate or glucose values.) It's read-only and stays on while a phone is connected.
  Hold for 2 seconds and release again to turn it off.

The log never includes your token, Nightscout address or WiFi passwords. It does include
WiFi network names.

## Adding another board

Any ESP32 with a display supported by [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) should work:

1. Copy one of the `board_*.h` files and fill in the pins. TFT_eSPI's `User_Setups/` folder
   has ready-made ones for many boards.
2. Add a line to the `case` in `build.sh` with its header and Arduino board ID (FQBN).

Drawing uses the screen's own width and height, so the sketch itself shouldn't need changes.
If the board gates display power on a pin (like the T-Display S3's GPIO15), define
`BOARD_POWER_PIN` in its header.

## How it works

Every 60 s it fetches the latest reading and computes its age against NTP time.

- **Nightscout:** `https://<host>/api/v1/entries/sgv.json?count=1`, keeping only `sgv`, `date`
  and `direction`. The certificate is verified against the standard root CAs built into the
  ESP32 core (the same ones browsers trust), so any normally hosted site works, and nobody on the
  network can read your token or show you a fake number. A self-signed certificate won't work.
- **Dexcom Share:** logs in once (account ID, then session ID), then reads the latest value with
  that session until it expires. Because this sends your password, the server certificate
  **is** verified, against the root CAs in `dexcom_roots.h`.

## Free

**Glucoled is free. Always.**

There's nothing to pay, and I don't accept donations. If Glucoled helps you, consider supporting a
diabetes charity of your choice instead, such as [Breakthrough T1D](https://www.breakthrought1d.org/) or the
[Japan IDDM Network](https://japan-iddm.net/).

I've lived with type 1 diabetes since 1992. I built Glucoled because I wanted something simple,
reliable and affordable, and I'm sharing it because others might find it useful too.
If it makes your life a little easier, that's enough for me.
You're always welcome to [say hello](https://github.com/mrizzo/glucoled/discussions).

## Related projects

- [M5StickC NightscoutMon](https://github.com/mlukasek/M5StickC_NightscoutMon) by mlukasek: a
  long-standing Nightscout monitor for M5Stack devices, with alarms and more on the screen.
  If you want those, it's an excellent choice.

## License

MIT. See [LICENSE](LICENSE).
