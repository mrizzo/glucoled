// TFT_eSPI setup for the ideaspark ESP32 1.9" ST7789 170x320 board (ESP32-WROOM-32, CH340).
// Force-included for every file (sketch + TFT_eSPI library) by build.sh via -include,
// so the library's own User_Setup.h is never used and never needs editing.
#pragma once
#define USER_SETUP_LOADED 1
#define ST7789_DRIVER
#define TFT_WIDTH  170
#define TFT_HEIGHT 320
#define TFT_MOSI 23
#define TFT_SCLK 18
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST   4
#define TFT_BL   32
#define TFT_BACKLIGHT_ON HIGH
// If colours look inverted (black shows white), swap these two lines:
#define TFT_INVERSION_ON
// #define TFT_INVERSION_OFF
// If red and blue are swapped, uncomment:
// #define TFT_RGB_ORDER TFT_BGR
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT7
#define SMOOTH_FONT
#define SPI_FREQUENCY  40000000
#define SPI_READ_FREQUENCY 20000000
