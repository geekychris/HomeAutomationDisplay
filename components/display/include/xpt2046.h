// XPT2046 resistive touch, mapped to screen pixels by a 3-point calibration
// kept in NVS.
#pragma once
#include <stdbool.h>

void xpt_init(void);
bool xpt_read_raw(int *rx, int *ry);   // panel coordinates, 0..4095
bool xpt_calibrated(void);
bool xpt_read(int *x, int *y);         // screen pixels; false if not touched or not calibrated
// Screen points and the raw readings taken there. False if the three readings
// cannot define a map (too close together, or in a line).
bool xpt_set_calibration(const int sx[3], const int sy[3], const int rx[3], const int ry[3]);
