#include "ui.h"
#include "sdkconfig.h"

ui_t UI;

#define F(n) (&lv_font_montserrat_##n)

void ui_init(int w, int h)
{
    UI.w = w;
    UI.h = h;
    const float sx = w / 320.0f, sy = h / 240.0f;
    UI.s = sx < sy ? sx : sy;
    UI.cols = w >= 800 ? 4 : 2;
    // The 320x240 set, always built (sdkconfig.defaults).
    UI.small = F(12); UI.body = F(14); UI.mid = F(20); UI.big = F(28); UI.huge = F(48);
#if CONFIG_LV_FONT_MONTSERRAT_16 && CONFIG_LV_FONT_MONTSERRAT_18 && CONFIG_LV_FONT_MONTSERRAT_24 && CONFIG_LV_FONT_MONTSERRAT_36
    if (UI.s >= 1.2f && UI.s < 2.0f) {
        UI.small = F(16); UI.body = F(18); UI.mid = F(24); UI.big = F(36);
    }
#endif
#if CONFIG_LV_FONT_MONTSERRAT_24 && CONFIG_LV_FONT_MONTSERRAT_28 && CONFIG_LV_FONT_MONTSERRAT_36
    // 48 is the biggest built-in Montserrat, so readings stop growing there.
    if (UI.s >= 2.0f) {
        UI.small = F(24); UI.body = F(28); UI.mid = F(36); UI.big = F(48);
    }
#endif
}
