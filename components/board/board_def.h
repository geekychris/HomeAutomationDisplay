// One board's wiring. Private to the board component: the rest of the
// firmware sees board.h only.
#pragma once
#include <stdbool.h>
#include "board.h"

typedef enum { LCD_ILI9341, LCD_ST7796, LCD_EK79007 } lcd_chip_t;

typedef enum {
    TOUCH_XPT2046_GPIO,     // resistive, bit-banged on pins of its own
    TOUCH_XPT2046_SPI,      // resistive, a second device on the LCD's SPI bus
    TOUCH_GT911,            // capacitive, I2C
    TOUCH_FT6336,           // capacitive, I2C (the FT5x06 family)
} touch_kind_t;

typedef struct {
    board_info_t info;

    // The panel. SPI pins are unused on MIPI DSI boards.
    lcd_chip_t lcd;
    int sclk, mosi, miso, cs, dc, rst;
    int pclk_mhz;
    bool bgr;               // colour order BGR rather than RGB
    bool invert;            // the panel needs its colours inverted
    bool swap_xy, mirror_x, mirror_y;   // native orientation -> landscape
    int bl;                 // backlight GPIO
    bool bl_low;            // backlight on when the pin is low

    // Touch.
    touch_kind_t touch;
    int t_sclk, t_mosi, t_miso;         // XPT2046_GPIO only
    int t_cs, t_irq;                    // XPT2046 (IRQ low = touched; -1: none)
    int t_sda, t_scl, t_int, t_rst;     // I2C touch (-1: not connected)
    int t_x_max, t_y_max;               // I2C touch: its own coordinate range
    bool t_swap_xy, t_mirror_x, t_mirror_y;   // I2C touch -> landscape screen
} board_def_t;

extern const board_def_t BOARD;
