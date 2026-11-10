// The touchscreen's WiFi screen: scan, join, remove. Opens over `back` and
// returns to it on Done. Call with the LVGL lock held (from a UI event).
#pragma once
#include "lvgl.h"
void wifi_setup_open(lv_obj_t *back);
