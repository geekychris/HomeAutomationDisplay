// The boards this firmware knows, one of which is built in (menuconfig:
// Home Automation Display -> Board). Pins and quirks come from each maker's
// schematics and demo code.
#include "board_def.h"
#include "sdkconfig.h"

#if CONFIG_HAD_BOARD_CYD28
// ESP32-2432S028R "Cheap Yellow Display" and clones (ELEGOO E32R28T).
// ILI9341 240x320, XPT2046 resistive touch on pins of its own.
const board_def_t BOARD = {
    .info = { "cyd28", "2.8\" Cheap Yellow Display", 320, 240 },
    .lcd = LCD_ILI9341, .sclk = 14, .mosi = 13, .miso = -1, .cs = 15, .dc = 2, .rst = -1,
    .pclk_mhz = 40, .bgr = true, .swap_xy = true,
    .bl = 21,
    .touch = TOUCH_XPT2046_GPIO, .t_sclk = 25, .t_mosi = 32, .t_miso = 39, .t_cs = 33, .t_irq = 36,
};

#elif CONFIG_HAD_BOARD_ST7796R || CONFIG_HAD_BOARD_SUNTON35C
// Sunton ESP32-3248S035R/C and ELEGOO / LCDwiki E32R35T, E32R40T: the same
// wiring. ST7796 320x480 (landscape: rows and columns swapped), reset tied to
// EN, backlight through a transistor on 27. Sunton's demo, LCDwiki's init and
// ESP3D-TFT all agree on BGR with no inversion.
#define ST7796_PANEL \
    .lcd = LCD_ST7796, .sclk = 14, .mosi = 13, .miso = 12, .cs = 15, .dc = 2, .rst = -1, \
    .pclk_mhz = 40, .bgr = true, .swap_xy = true, .bl = 27

#if CONFIG_HAD_BOARD_ST7796R
// The resistive ones: XPT2046 on the LCD's SPI bus, CS 33, pen IRQ 36.
const board_def_t BOARD = {
    .info = { "st7796r", "3.5\"/4.0\" ST7796 (resistive)", 480, 320 },
    ST7796_PANEL,
    .touch = TOUCH_XPT2046_SPI, .t_cs = 33, .t_irq = 36,
};
#else
// The capacitive one: GT911 on I2C. Its INT pin is not reliably connected,
// so it is polled, and touch.c asks both of its possible addresses.
const board_def_t BOARD = {
    .info = { "sunton35c", "3.5\" Sunton ESP32-3248S035C", 480, 320 },
    ST7796_PANEL,
    .touch = TOUCH_GT911, .t_sda = 33, .t_scl = 32, .t_int = -1, .t_rst = 25,
    .t_x_max = 320, .t_y_max = 480, .t_swap_xy = true,
};
#endif

#else
#error "No board entry for the board chosen in menuconfig"
#endif
