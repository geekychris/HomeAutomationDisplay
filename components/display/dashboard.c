// The dashboard: the hub's tiles, one page at a time.
//
//   [<]  Page name  [>]   12:34  o  [gear]     30 px header
//   +-----------+ +-----------+
//   | Kitchen   | | CO2       |                tiles 151 x 99, two across;
//   | On  70%   | | 812 ppm   |                a thermostat takes a full row
//   | ===o----- | | Good      |
//   +-----------+ +-----------+
//
// Only the current page's tiles exist as LVGL objects; the rest of the
// layout lives in the hub model. An LVGL timer watches the model and
// rebuilds the page when a new layout arrives, or redraws a tile when its
// state changes.
#include "dashboard.h"
#include "hub.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "netcfg.h"

#define W 320
#define H 240
#define BAR_H 30
#define TILE_W 151
#define TILE_H 99
#define PER_PAGE 8                  // the hub's MAX_TILES_PER_PAGE
#define SEND_AFTER_MS 800           // thermostat: send the target once the taps stop

#define C_BG     0x15171a
#define C_BAR    0x22262b
#define C_CARD   0x2a2f35
#define C_CARD_ON 0x5a4313
#define C_ACCENT 0xf5a623
#define C_MUTED  0xb0b0b0
#define C_GOOD   0x3fb950
#define C_BAD    0xef5350
#define C_BTN    0x3a4048

typedef struct {
    int idx;                        // into hub_layout()->tiles
    uint32_t seq;
    hub_kind_t kind;
    char entity[HUB_ENTITY_LEN];
    bool dimmable;
    lv_obj_t *card, *name, *sub, *slider, *sw, *val, *unit, *lvl, *tgt, *mode;
    // Thermostat: what the last state said, and a target being set here.
    float tgt_v, tmin, tmax, step;
    char modes[HUB_MAX_MODES][10];
    int nmodes, cur_mode;
    lv_timer_t *send_timer;
    lv_timer_t *flash_timer;        // button: the tap's flash
} ui_tile_t;

static ui_tile_t s_ui[PER_PAGE];
static int s_nui;
static uint32_t s_gen = UINT32_MAX;
static int s_page;
static lv_obj_t *s_grid, *s_title, *s_prev, *s_next, *s_clock, *s_dot, *s_msg;
static lv_obj_t *s_overlay, *s_toast;
static lv_timer_t *s_overlay_timer, *s_toast_timer;
static void (*s_on_settings)(void);
static lv_style_t st_card, st_name, st_small, st_btn;
static char s_title_txt[HUB_NAME_LEN];

// ---------------------------------------------------------------- small things

static void toast(const char *text)
{
    lv_label_set_text(lv_obj_get_child(s_toast, 0), text);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_timer_reset(s_toast_timer);
    lv_timer_resume(s_toast_timer);
}
static void toast_hide(lv_timer_t *t) { lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN); lv_timer_pause(t); }

static void not_sent(bool ok) { if (!ok) toast("Not connected to the hub"); }

static lv_obj_t *label(lv_obj_t *parent, lv_style_t *st, const lv_font_t *font)
{
    lv_obj_t *l = lv_label_create(parent);
    if (st) lv_obj_add_style(l, st, 0);
    if (font) lv_obj_set_style_text_font(l, font, 0);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *flat_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *arg)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_add_style(b, &st_btn, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, arg);
    return b;
}

static void set_text(lv_obj_t *l, const char *text)
{
    if (strcmp(lv_label_get_text(l), text)) lv_label_set_text(l, text);
}

static const char *mode_name(const char *m)
{
    return !strcmp(m, "heat") ? "Heat" : !strcmp(m, "cool") ? "Cool" : !strcmp(m, "off") ? "Off"
         : !strcmp(m, "auto") || !strcmp(m, "heat_cool") ? "Auto" : !strcmp(m, "dry") ? "Dry"
         : !strcmp(m, "fan_only") ? "Fan" : m;
}

// The built-in symbol font has no lock, so locks keep the power symbol.
static const char *icon_symbol(hub_icon_t ic)
{
    switch (ic) {
    case HUB_IC_FAN:   return LV_SYMBOL_REFRESH;
    case HUB_IC_COVER: return LV_SYMBOL_UP LV_SYMBOL_DOWN;
    case HUB_IC_BELL:  return LV_SYMBOL_BELL;
    case HUB_IC_PLAY:  return LV_SYMBOL_PLAY;
    default:           return LV_SYMBOL_POWER;
    }
}

// ---------------------------------------------------------------- taps

static void on_card(lv_event_t *e)
{
    ui_tile_t *u = lv_event_get_user_data(e);
    not_sent(hub_toggle(u->entity));
}

static void on_slider(lv_event_t *e)
{
    ui_tile_t *u = lv_event_get_user_data(e);
    const int v = lv_slider_get_value(u->slider);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_label_set_text_fmt(u->sub, "%d%%", v);   // a level: brightness, fan speed or position
    } else {
        not_sent(hub_brightness(u->entity, v));
    }
}

static void flash_end(lv_timer_t *t)
{
    ui_tile_t *u = lv_timer_get_user_data(t);
    u->flash_timer = NULL;              // one-shot: LVGL deletes it after this
    lv_obj_set_style_bg_color(u->card, lv_color_hex(C_CARD), 0);
    u->seq--;                           // redraw from the model on the next watch
}

static void on_press(lv_event_t *e)
{
    ui_tile_t *u = lv_event_get_user_data(e);
    const bool ok = hub_press(u->entity);
    not_sent(ok);
    if (!ok || u->flash_timer) return;
    lv_obj_set_style_bg_color(u->card, lv_color_hex(C_CARD_ON), 0);
    u->flash_timer = lv_timer_create(flash_end, 300, u);
    lv_timer_set_repeat_count(u->flash_timer, 1);
}

static void send_target(lv_timer_t *t)
{
    ui_tile_t *u = lv_timer_get_user_data(t);
    u->send_timer = NULL;               // a one-shot timer: LVGL deletes it after this
    not_sent(hub_target(u->entity, u->tgt_v));
}

static void nudge(ui_tile_t *u, int dir)
{
    if (isnan(u->tgt_v)) return;
    float v = roundf((u->tgt_v + dir * u->step) / u->step) * u->step;
    v = v < u->tmin ? u->tmin : v > u->tmax ? u->tmax : v;
    u->tgt_v = v;
    lv_label_set_text_fmt(u->tgt, "%.1f°", (double)v);
    if (u->send_timer) {
        lv_timer_reset(u->send_timer);
    } else {
        u->send_timer = lv_timer_create(send_target, SEND_AFTER_MS, u);
        lv_timer_set_repeat_count(u->send_timer, 1);
    }
}
static void on_minus(lv_event_t *e) { nudge(lv_event_get_user_data(e), -1); }
static void on_plus(lv_event_t *e)  { nudge(lv_event_get_user_data(e), +1); }

static void on_mode(lv_event_t *e)
{
    ui_tile_t *u = lv_event_get_user_data(e);
    if (!u->nmodes) return;
    const int next = (u->cur_mode + 1) % u->nmodes;
    lv_label_set_text(lv_obj_get_child(u->mode, 0), mode_name(u->modes[next]));
    not_sent(hub_mode(u->entity, u->modes[next]));
}

// ---------------------------------------------------------------- drawing a tile's state

static void update(ui_tile_t *u, const hub_tile_t *t)
{
    char b[32];
    lv_obj_set_style_opa(u->card, t->ok ? LV_OPA_COVER : LV_OPA_50, 0);
    switch (u->kind) {
    case HUB_LIGHT:
    case HUB_SWITCH:
        lv_obj_set_style_bg_color(u->card, lv_color_hex(t->ok && t->on ? C_CARD_ON : C_CARD), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(u->card, 1),
                                    lv_color_hex(t->ok && t->on ? C_ACCENT : 0x808080), 0);
        if (!t->ok)                     snprintf(b, sizeof(b), "unavailable");
        else if (t->txt[0])             snprintf(b, sizeof(b), "%s", t->txt);
        else if (t->on && u->dimmable)  snprintf(b, sizeof(b), "On  %d%%", t->bri);
        else                            snprintf(b, sizeof(b), "%s", t->on ? "On" : "Off");
        // Leave a slider being dragged alone.
        if (u->slider && !lv_obj_has_state(u->slider, LV_STATE_PRESSED)) {
            set_text(u->sub, b);
            lv_slider_set_value(u->slider, t->on ? t->bri : 0, LV_ANIM_OFF);
            if (t->ok) lv_obj_remove_state(u->slider, LV_STATE_DISABLED);
            else lv_obj_add_state(u->slider, LV_STATE_DISABLED);
        } else if (!u->slider) {
            set_text(u->sub, b);
        }
        if (u->sw) {
            if (t->on) lv_obj_add_state(u->sw, LV_STATE_CHECKED);
            else lv_obj_remove_state(u->sw, LV_STATE_CHECKED);
        }
        break;
    case HUB_SENSOR: {
        const uint32_t c = t->lvl == 1 ? C_GOOD : t->lvl == 2 ? C_ACCENT : t->lvl == 3 ? C_BAD : 0xffffff;
        lv_obj_set_style_text_color(u->val, lv_color_hex(c), 0);
        lv_obj_set_style_text_color(u->lvl, lv_color_hex(t->lvl ? c : C_MUTED), 0);
        set_text(u->val, t->ok ? t->val : "--");
        set_text(u->unit, t->ok ? t->unit : "");
        lv_obj_align_to(u->unit, u->val, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -4);
        static const char *const LV[] = { "", "Good", "Fair", "Poor" };
        set_text(u->lvl, !t->ok ? "unavailable" : t->txt[0] ? t->txt : LV[t->lvl & 3]);
        break;
    }
    case HUB_BUTTON:
        if (!u->flash_timer)
            lv_obj_set_style_bg_color(u->card, lv_color_hex(t->ok && t->on ? C_CARD_ON : C_CARD), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(u->card, 1),
                                    lv_color_hex(t->ok && t->on ? C_ACCENT : 0x808080), 0);
        set_text(u->sub, !t->ok ? "unavailable" : t->txt[0] ? t->txt : "Press");
        break;
    case HUB_CLIMATE:
        if (!t->ok || isnan(t->cur)) snprintf(b, sizeof(b), "--");
        else snprintf(b, sizeof(b), "%.1f°", (double)t->cur);
        set_text(u->val, b);
        const bool heating = !strcmp(t->act, "heating"), cooling = !strcmp(t->act, "cooling");
        set_text(u->lvl, !t->ok ? "unavailable" : heating ? "Heating" : cooling ? "Cooling"
                         : !strcmp(t->mode, "off") ? "Off" : "Idle");
        lv_obj_set_style_text_color(u->lvl, lv_color_hex(heating || cooling ? C_ACCENT : C_MUTED), 0);
        u->tmin = t->tmin; u->tmax = t->tmax; u->step = t->step > 0 ? t->step : 0.5f;
        u->nmodes = t->nmodes;
        u->cur_mode = 0;
        for (int i = 0; i < t->nmodes; i++) {
            snprintf(u->modes[i], sizeof(u->modes[i]), "%s", t->modes[i]);
            if (!strcmp(t->modes[i], t->mode)) u->cur_mode = i;
        }
        set_text(lv_obj_get_child(u->mode, 0), t->ok ? mode_name(t->mode) : "--");
        lv_obj_set_style_bg_color(u->mode, lv_color_hex(!strcmp(t->mode, "heat") ? 0x8a5a12 : C_BTN), 0);
        if (!u->send_timer) {           // not while a new target is being set here
            u->tgt_v = t->tgt;
            if (!t->ok || isnan(t->tgt)) snprintf(b, sizeof(b), "--");
            else snprintf(b, sizeof(b), "%.1f°", (double)t->tgt);
            set_text(u->tgt, b);
        }
        break;
    }
}

// ---------------------------------------------------------------- building a page

static void build_tile(ui_tile_t *u, const hub_tile_t *t)
{
    u->kind = t->kind;
    u->dimmable = t->dimmable;
    snprintf(u->entity, sizeof(u->entity), "%s", t->entity);
    u->card = lv_obj_create(s_grid);
    lv_obj_add_style(u->card, &st_card, 0);
    lv_obj_set_size(u->card, t->kind == HUB_CLIMATE ? W - 12 : TILE_W, TILE_H);
    lv_obj_remove_flag(u->card, LV_OBJ_FLAG_SCROLLABLE);
    u->name = label(u->card, t->kind == HUB_LIGHT || t->kind == HUB_SWITCH || t->kind == HUB_BUTTON
                             ? &st_name : &st_small, NULL);
    lv_label_set_text(u->name, t->name);
    lv_label_set_long_mode(u->name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(u->name, t->kind == HUB_CLIMATE ? 140 : TILE_W - 40);

    switch (t->kind) {
    case HUB_LIGHT:
    case HUB_SWITCH: {
        lv_obj_t *pwr = label(u->card, NULL, NULL);       // child 1: coloured when on
        lv_label_set_text(pwr, icon_symbol(t->icon));
        lv_obj_align(pwr, LV_ALIGN_TOP_RIGHT, 0, 0);
        u->sub = label(u->card, &st_small, NULL);
        lv_obj_align(u->sub, LV_ALIGN_TOP_LEFT, 0, 22);
        lv_obj_add_flag(u->card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(u->card, on_card, LV_EVENT_CLICKED, u);
        if (t->kind == HUB_LIGHT && t->dimmable) {
            u->slider = lv_slider_create(u->card);
            lv_slider_set_range(u->slider, 0, 100);
            lv_obj_set_size(u->slider, TILE_W - 36, 8);
            lv_obj_align(u->slider, LV_ALIGN_BOTTOM_MID, 0, -6);
            lv_obj_set_ext_click_area(u->slider, 12);
            lv_obj_set_style_bg_color(u->slider, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(u->slider, lv_color_hex(0xffffff), LV_PART_KNOB);
            lv_obj_add_event_cb(u->slider, on_slider, LV_EVENT_VALUE_CHANGED, u);
            lv_obj_add_event_cb(u->slider, on_slider, LV_EVENT_RELEASED, u);
        } else if (t->kind == HUB_SWITCH) {
            u->sw = lv_switch_create(u->card);
            lv_obj_set_size(u->sw, 50, 26);
            lv_obj_align(u->sw, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
            lv_obj_remove_flag(u->sw, LV_OBJ_FLAG_CLICKABLE);   // the whole card is the button
            lv_obj_set_style_bg_color(u->sw, lv_color_hex(C_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
        }
        break;
    }
    case HUB_BUTTON: {
        lv_obj_t *ic = label(u->card, NULL, NULL);        // child 1: coloured while running
        lv_label_set_text(ic, icon_symbol(t->icon));
        lv_obj_align(ic, LV_ALIGN_TOP_RIGHT, 0, 0);
        u->sub = label(u->card, &st_small, NULL);
        lv_obj_align(u->sub, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_add_flag(u->card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(u->card, on_press, LV_EVENT_CLICKED, u);
        break;
    }
    case HUB_SENSOR:
        u->val = label(u->card, NULL, &lv_font_montserrat_28);
        lv_obj_align(u->val, LV_ALIGN_TOP_LEFT, 0, 22);
        u->unit = label(u->card, &st_small, &lv_font_montserrat_14);
        u->lvl = label(u->card, &st_small, NULL);
        lv_obj_align(u->lvl, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        break;
    case HUB_CLIMATE: {
        u->val = label(u->card, NULL, &lv_font_montserrat_28);       // current temperature
        lv_obj_align(u->val, LV_ALIGN_TOP_LEFT, 0, 20);
        u->lvl = label(u->card, &st_small, NULL);                     // heating / idle
        lv_obj_align(u->lvl, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_t *minus = flat_button(u->card, LV_SYMBOL_MINUS, on_minus, u);
        lv_obj_set_size(minus, 40, 38);
        lv_obj_set_pos(minus, 150, 0);
        u->tgt = label(u->card, NULL, &lv_font_montserrat_20);
        lv_obj_set_width(u->tgt, 62);
        lv_obj_set_style_text_align(u->tgt, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(u->tgt, 190, 9);
        lv_obj_t *plus = flat_button(u->card, LV_SYMBOL_PLUS, on_plus, u);
        lv_obj_set_size(plus, 40, 38);
        lv_obj_set_pos(plus, 252, 0);
        u->mode = flat_button(u->card, "--", on_mode, u);
        lv_obj_set_size(u->mode, 142, 32);
        lv_obj_set_pos(u->mode, 150, 51);
        break;
    }
    }
    u->seq = t->seq;
    update(u, t);
}

static void clear_page(void)
{
    for (int i = 0; i < s_nui; i++) {
        if (s_ui[i].send_timer) lv_timer_delete(s_ui[i].send_timer);
        if (s_ui[i].flash_timer) lv_timer_delete(s_ui[i].flash_timer);
    }
    memset(s_ui, 0, sizeof(s_ui));
    s_nui = 0;
    lv_obj_clean(s_grid);
    s_msg = NULL;
}

// Call with the hub lock held.
static void build_page(const hub_layout_t *L)
{
    clear_page();
    if (L->npages == 0) {
        s_msg = label(s_grid, &st_small, &lv_font_montserrat_14);
        lv_obj_set_width(s_msg, W - 24);
        lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(s_msg, 60, 0);
    } else {
        for (int i = 0; i < L->ntiles && s_nui < PER_PAGE; i++) {
            if (L->tiles[i].page != s_page) continue;
            s_ui[s_nui].idx = i;
            build_tile(&s_ui[s_nui++], &L->tiles[i]);
        }
        if (!s_nui) {
            s_msg = label(s_grid, &st_small, NULL);
            lv_label_set_text(s_msg, "Nothing on this page.");
        }
    }
    snprintf(s_title_txt, sizeof(s_title_txt), "%s",
             L->npages > 1 ? L->page[s_page] : L->title[0] ? L->title : hub_device_id());
    lv_label_set_text(s_title, s_title_txt);
    const bool many = L->npages > 1;
    if (many) { lv_obj_remove_flag(s_prev, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(s_next, LV_OBJ_FLAG_HIDDEN); }
    else      { lv_obj_add_flag(s_prev, LV_OBJ_FLAG_HIDDEN);    lv_obj_add_flag(s_next, LV_OBJ_FLAG_HIDDEN); }
    lv_obj_scroll_to_y(s_grid, 0, LV_ANIM_OFF);
}

static void turn_page(int by)
{
    hub_lock();
    const hub_layout_t *L = hub_layout();
    if (L->npages > 1) {
        s_page = (s_page + L->npages + by) % L->npages;
        build_page(L);
    }
    hub_unlock();
}
static void on_prev(lv_event_t *e) { turn_page(-1); }
static void on_next(lv_event_t *e) { turn_page(+1); }
static void on_gear(lv_event_t *e) { if (s_on_settings) s_on_settings(); }

// ---------------------------------------------------------------- identify

static void overlay_hide(lv_timer_t *t) { lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN); lv_timer_pause(t); }
static void on_overlay(lv_event_t *e) { overlay_hide(s_overlay_timer); }

static void identify(int seconds)
{
    lv_label_set_text_fmt(lv_obj_get_child(s_overlay, 1), "%s", hub_device_id());
    lv_label_set_text_fmt(lv_obj_get_child(s_overlay, 2), "%s\n%s.local", s_title_txt, netcfg_hostname());
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_timer_set_period(s_overlay_timer, seconds * 1000);
    lv_timer_reset(s_overlay_timer);
    lv_timer_resume(s_overlay_timer);
}

// ---------------------------------------------------------------- the watch timer

static void watch(lv_timer_t *timer)
{
    hub_lock();
    const hub_layout_t *L = hub_layout();
    if (L->gen != s_gen) {
        s_gen = L->gen;
        if (s_page >= L->npages) s_page = 0;
        build_page(L);
    }
    for (int i = 0; i < s_nui; i++) {
        const hub_tile_t *t = &L->tiles[s_ui[i].idx];
        if (t->seq != s_ui[i].seq) { s_ui[i].seq = t->seq; update(&s_ui[i], t); }
    }
    if (s_msg && L->npages == 0) {
        char b[112];
        if (L->msg[0] && hub_status() == HUB_ONLINE) snprintf(b, sizeof(b), "%s", L->msg);
        else if (!netcfg_connected()) snprintf(b, sizeof(b), "No WiFi yet.\nSet it up under " LV_SYMBOL_SETTINGS " .");
        else { hub_status_text(b, sizeof(b)); strncat(b, "...", sizeof(b) - strlen(b) - 1); }
        set_text(s_msg, b);
    }
    hub_unlock();

    const int id_s = hub_take_identify();
    if (id_s) identify(id_s);
    char err[96];
    if (hub_take_error(err, sizeof(err))) toast(err);

    lv_obj_set_style_bg_color(s_dot, lv_color_hex(hub_status() == HUB_ONLINE ? C_GOOD : C_BAD), 0);
    if (netcfg_time_valid()) {
        const time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char b[8];
        strftime(b, sizeof(b), "%H:%M", &tm);
        set_text(s_clock, b);
    }
}

// ---------------------------------------------------------------- start

static void styles(void)
{
    lv_style_init(&st_card);
    lv_style_set_bg_color(&st_card, lv_color_hex(C_CARD));
    lv_style_set_bg_opa(&st_card, LV_OPA_COVER);
    lv_style_set_border_width(&st_card, 0);
    lv_style_set_radius(&st_card, 8);
    lv_style_set_pad_all(&st_card, 8);
    lv_style_set_text_color(&st_card, lv_color_hex(0xf0f0f0));
    lv_style_init(&st_name);
    lv_style_set_text_font(&st_name, &lv_font_montserrat_14);
    lv_style_init(&st_small);
    lv_style_set_text_font(&st_small, &lv_font_montserrat_12);
    lv_style_set_text_color(&st_small, lv_color_hex(C_MUTED));
    lv_style_init(&st_btn);
    lv_style_set_bg_color(&st_btn, lv_color_hex(C_BTN));
    lv_style_set_radius(&st_btn, 6);
    lv_style_set_shadow_width(&st_btn, 0);
    lv_style_set_pad_all(&st_btn, 0);
}

static lv_obj_t *bar_button(lv_obj_t *bar, const char *sym, lv_event_cb_t cb, int x)
{
    lv_obj_t *b = lv_button_create(bar);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 30, BAR_H);
    lv_obj_set_pos(b, x, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_color(l, lv_color_hex(C_ACCENT), 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

void dashboard_build(lv_obj_t *scr, void (*on_settings)(void))
{
    s_on_settings = on_settings;
    styles();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, W, BAR_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_BAR), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    s_prev = bar_button(bar, LV_SYMBOL_LEFT, on_prev, 0);
    s_title = lv_label_create(bar);
    lv_obj_set_width(s_title, 140);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_title, 30, 7);
    lv_label_set_text(s_title, "");
    s_next = bar_button(bar, LV_SYMBOL_RIGHT, on_next, 170);
    s_clock = label(bar, &st_small, &lv_font_montserrat_14);
    lv_label_set_text(s_clock, "--:--");
    lv_obj_set_pos(s_clock, 222, 7);
    s_dot = lv_obj_create(bar);
    lv_obj_remove_style_all(s_dot);
    lv_obj_set_size(s_dot, 8, 8);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
    lv_obj_set_pos(s_dot, 272, 11);
    bar_button(bar, LV_SYMBOL_SETTINGS, on_gear, W - 34);

    s_grid = lv_obj_create(scr);
    lv_obj_remove_style_all(s_grid);
    lv_obj_set_pos(s_grid, 0, BAR_H);
    lv_obj_set_size(s_grid, W, H - BAR_H);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_all(s_grid, 6, 0);
    lv_obj_set_style_pad_gap(s_grid, 6, 0);
    lv_obj_set_scroll_dir(s_grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_grid, LV_SCROLLBAR_MODE_ACTIVE);

    // On top of everything: the identify card and the error toast.
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, W, H);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x0a0a0a), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_90, 0);
    lv_obj_set_flex_flow(s_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_overlay, 8, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_overlay, on_overlay, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(label(s_overlay, NULL, NULL), "This is");
    lv_obj_t *big = label(s_overlay, NULL, &lv_font_montserrat_28);
    lv_obj_set_style_text_color(big, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_text_align(label(s_overlay, &st_small, &lv_font_montserrat_14), LV_TEXT_ALIGN_CENTER, 0);
    s_overlay_timer = lv_timer_create(overlay_hide, 10000, NULL);
    lv_timer_pause(s_overlay_timer);

    s_toast = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_toast);
    lv_obj_set_size(s_toast, W - 20, LV_SIZE_CONTENT);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x7a2a22), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_toast, 6, 0);
    lv_obj_set_style_pad_all(s_toast, 6, 0);
    lv_obj_t *tl = label(s_toast, NULL, &lv_font_montserrat_12);
    lv_obj_set_width(tl, lv_pct(100));
    lv_label_set_long_mode(tl, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    s_toast_timer = lv_timer_create(toast_hide, 3000, NULL);
    lv_timer_pause(s_toast_timer);

    watch(NULL);
    lv_timer_create(watch, 100, NULL);
}
