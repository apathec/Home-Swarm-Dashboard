// =====================================================================
// HOSYOND 4.0" ESP32-32E ST7796S 480x320 — TFT_eSPI User_Setup.h snippet
// =====================================================================
// Replace the contents of:
//   Documents/Arduino/libraries/TFT_eSPI/User_Setup.h
// with the contents of this file.

#define USER_SETUP_INFO "Hosyond 4.0 ST7796S"

#define ST7796_DRIVER

#define TFT_WIDTH  320
#define TFT_HEIGHT 480

// SPI pins (HSPI on the ESP32-32E variant of this board)
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  -1
#define TFT_BL   27
#define TFT_BACKLIGHT_ON HIGH

#define TOUCH_CS 33

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

#define SPI_FREQUENCY        40000000
#define SPI_READ_FREQUENCY   20000000
#define SPI_TOUCH_FREQUENCY   2500000

#define USE_HSPI_PORT
