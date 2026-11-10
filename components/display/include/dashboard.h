// The main screen: the tiles the hub assigned to this display, a page at a
// time, with page arrows, the clock, the hub link's status and a settings
// button in a bar across the top. Call once, with the LVGL lock held.
#pragma once
#include "lvgl.h"
void dashboard_build(lv_obj_t *scr, void (*on_settings)(void));
