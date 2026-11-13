// Sizes for the screen this board has. The UI was drawn for 320x240; on a
// bigger screen everything grows by one factor (the same 8 tiles a page,
// bigger), and fonts step up to the nearest built-in Montserrat.
#pragma once
#include "lvgl.h"

typedef struct {
    int w, h;                   // the screen, landscape
    float s;                    // scale: 1 on 320x240, 1.33 on 480x320, 2.5 on 1024x600
    int cols;                   // tiles across: 2, or 4 on wide screens (all 8 at once)
    const lv_font_t *small;     // secondary text (12 at 320x240)
    const lv_font_t *body;      // names, buttons (14)
    const lv_font_t *mid;       // titles, a thermostat's target (20)
    const lv_font_t *big;       // readings (28)
    const lv_font_t *huge;      // the identify overlay (48)
} ui_t;

extern ui_t UI;

void ui_init(int w, int h);

// A length drawn for 320x240, on this screen.
static inline int PX(int n) { return (int)(n * UI.s + (n >= 0 ? 0.5f : -0.5f)); }
