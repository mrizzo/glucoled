// LilyGO T-Display S3 (ESP32-S3, 1.9" ST7789 170x320, 8-bit parallel bus).
// Same as TFT_eSPI's User_Setups/Setup206_LilyGo_T_Display_S3.h; force-included by build.sh.
#pragma once
#define USER_SETUP_LOADED 1
#define ST7789_DRIVER
#define INIT_SEQUENCE_3
#define CGRAM_OFFSET
#define TFT_RGB_ORDER TFT_RGB
#define TFT_INVERSION_ON
#define TFT_PARALLEL_8_BIT
#define TFT_WIDTH  170
#define TFT_HEIGHT 320
#define TFT_CS   6
#define TFT_DC   7
#define TFT_RST  5
#define TFT_WR   8
#define TFT_RD   9
#define TFT_D0  39
#define TFT_D1  40
#define TFT_D2  41
#define TFT_D3  42
#define TFT_D4  45
#define TFT_D5  46
#define TFT_D6  47
#define TFT_D7  48
#define TFT_BL  38
#define TFT_BACKLIGHT_ON HIGH
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT7
#define SMOOTH_FONT
// GPIO15 powers the screen: must be driven HIGH or the display stays dark on battery.
#define BOARD_POWER_PIN 15
