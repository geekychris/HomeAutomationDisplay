// The board: which panel, which touch controller, and how they are wired.
// Everything else (the dashboard, setup screens, hub link) only sees the
// screen size and these calls, so a new board is a new entry in board.c.
//
// The board is chosen in menuconfig (Home Automation Display -> Board), and
// each board has its own sdkconfig defaults under boards/.
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

typedef struct {
    const char *id;         // short, sent to the hub: "cyd28", "sunton35c", ...
    const char *name;       // for people: "2.8\" Cheap Yellow Display"
    int w, h;               // the screen as LVGL sees it (landscape), in pixels
} board_info_t;

const board_info_t *board(void);

// Bring up the panel and add it to esp_lvgl_port. Call after lvgl_port_init(),
// without the LVGL lock. The backlight stays off until board_backlight().
lv_display_t *board_display_start(void);
void board_backlight(bool on);

// ---------------------------------------------------------------- touch

esp_err_t board_touch_init(void);
// Resistive panels need a 3-point calibration (kept in NVS); capacitive ones
// report screen coordinates already.
bool board_touch_resistive(void);
bool board_touch_calibrated(void);
// Screen pixels. False if not touched (or a resistive panel not calibrated yet).
bool board_touch_read(int *x, int *y);
// Resistive only: raw readings, for calibrating.
bool board_touch_read_raw(int *rx, int *ry);
// Screen points and the raw readings taken there. False if the three
// readings cannot define a map (too close together, or in a line).
bool board_touch_set_calibration(const int sx[3], const int sy[3], const int rx[3], const int ry[3]);
