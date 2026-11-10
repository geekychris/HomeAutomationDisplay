// The touchscreen: an ILI9341 320x240 panel and XPT2046 touch, driven by LVGL.
// Starts nothing (and returns an error) if there is not enough RAM, rather
// than letting LVGL hang on a failed allocation.
#pragma once
#include "esp_err.h"
esp_err_t display_start(void);
