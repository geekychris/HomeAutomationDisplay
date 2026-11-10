// WiFi setup on the touchscreen: scan, pick a network, type its password,
// and remove saved ones. The same netcfg calls as the setup web page, so a
// network added here behaves exactly like one added there.
//
// The screen is built when opened and deleted when closed: the keyboard and
// lists are only worth their RAM while someone is using them.
#include "wifi_setup.h"
#include "netcfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lvgl_port.h"

#define MAX_NETS 16

static lv_obj_t *s_back, *s_scr, *s_status, *s_list, *s_scan_btn;
static lv_obj_t *s_pw_scr, *s_pw_ta, *s_pw_title;
static netcfg_ap_t s_nets[MAX_NETS];
static int       s_n;
static char      s_pick[33];
static bool      s_scanning;
static lv_timer_t *s_timer;

static void set_status(const char *fmt, const char *arg)
{
    lv_label_set_text_fmt(s_status, fmt, arg ? arg : "");
}

// ---------------------------------------------------------------- the lists

static void on_join(lv_event_t *e);
static void on_forget(lv_event_t *e);

static const char *bars(int rssi)
{
    return rssi > -55 ? "****" : rssi > -67 ? "***" : rssi > -78 ? "**" : "*";
}

// Saved networks first (each with a remove button), then what the last scan
// found. Rebuilt whole each time; it is a dozen rows at most.
static void fill(void)
{
    lv_obj_clean(s_list);
    char saved[NETCFG_MAX][33];
    const int ns = netcfg_saved(saved, NETCFG_MAX);
    if (ns) lv_list_add_text(s_list, "Saved - tried in turn");
    for (int i = 0; i < ns; i++) {
        lv_obj_t *b = lv_list_add_button(s_list, LV_SYMBOL_WIFI, saved[i]);
        lv_obj_t *x = lv_button_create(b);
        lv_obj_set_size(x, 36, 26);
        lv_obj_set_style_bg_color(x, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
        lv_obj_t *l = lv_label_create(x);
        lv_label_set_text(l, LV_SYMBOL_TRASH);
        lv_obj_center(l);
        // The name rides along in the label; on_forget reads it back.
        lv_obj_add_event_cb(x, on_forget, LV_EVENT_CLICKED, NULL);
    }

    if (s_n) lv_list_add_text(s_list, "Found nearby - tap to join");
    for (int i = 0; i < s_n; i++) {
        char t[64];
        snprintf(t, sizeof(t), "%s   %s%s", s_nets[i].ssid, bars(s_nets[i].rssi),
                 s_nets[i].open ? "" : "  " LV_SYMBOL_EYE_CLOSE);
        lv_obj_t *b = lv_list_add_button(s_list, NULL, t);
        lv_obj_add_event_cb(b, on_join, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

// Most changes come from a tap on a row of the list itself, and a widget
// cannot be deleted while its own event is being handled - so the rebuild
// waits for LVGL to finish with the tap.
static void fill_later_cb(void *arg) { if (s_scr) fill(); }
static void fill_later(void) { lv_async_call(fill_later_cb, NULL); }

// ---------------------------------------------------------------- scanning

static void scan_task(void *arg)
{
    netcfg_ap_t found[MAX_NETS];
    int n = netcfg_scan(found, MAX_NETS);
    if (n < 0) n = 0;

    lvgl_port_lock(0);
    s_scanning = false;
    if (s_scr) {                         // still open
        memcpy(s_nets, found, sizeof(found));
        s_n = n;
        lv_obj_remove_state(s_scan_btn, LV_STATE_DISABLED);
        set_status(n ? "%s" : "Nothing found. The radio is busy while it joins a network - try again.", "");
        fill();
    }
    lvgl_port_unlock();
    vTaskDelete(NULL);
}

static void scan(void)
{
    if (s_scanning) return;
    s_scanning = true;
    lv_obj_add_state(s_scan_btn, LV_STATE_DISABLED);
    set_status("Scanning...%s", "");
    if (xTaskCreate(scan_task, "wifi_scan", 4096, NULL, 3, NULL) != pdPASS) {
        s_scanning = false;
        lv_obj_remove_state(s_scan_btn, LV_STATE_DISABLED);
        set_status("Not enough memory to scan%s", "");
    }
}

static void on_scan(lv_event_t *e) { scan(); }

// ---------------------------------------------------------------- saving

static void save(const char *ssid, const char *pass)
{
    const esp_err_t e = netcfg_add(ssid, pass);
    if (e == ESP_ERR_NO_MEM)      set_status("Not saved: %s", "the list is full - remove one first");
    else if (e != ESP_OK)         set_status("Not saved: %s", esp_err_to_name(e));
    else if (!netcfg_connected()) set_status("Saved. Joining %s...", ssid);
    else                          set_status("Saved %s. It is tried whenever the current network is out of reach.", ssid);
    fill_later();
}

static void close_pw(void)
{
    lv_screen_load(s_scr);
    lv_obj_delete_async(s_pw_scr);       // we are inside its keyboard's event
    s_pw_scr = NULL;
}

static void on_kb(lv_event_t *e)
{
    const lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY) {
        char pass[65];
        snprintf(pass, sizeof(pass), "%s", lv_textarea_get_text(s_pw_ta));
        close_pw();
        save(s_pick, pass);
    } else if (c == LV_EVENT_CANCEL) {
        close_pw();
    }
}

static void on_show(lv_event_t *e)
{
    lv_textarea_set_password_mode(s_pw_ta, !lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
}

static void ask_password(const char *ssid)
{
    snprintf(s_pick, sizeof(s_pick), "%s", ssid);
    s_pw_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_pw_scr, LV_OBJ_FLAG_SCROLLABLE);
    s_pw_title = lv_label_create(s_pw_scr);
    lv_label_set_text_fmt(s_pw_title, "Password for %s", ssid);
    lv_label_set_long_mode(s_pw_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_pw_title, 300);
    lv_obj_set_pos(s_pw_title, 8, 6);
    s_pw_ta = lv_textarea_create(s_pw_scr);
    lv_textarea_set_one_line(s_pw_ta, true);
    lv_textarea_set_password_mode(s_pw_ta, true);
    lv_textarea_set_max_length(s_pw_ta, 64);
    lv_obj_set_size(s_pw_ta, 220, 38);
    lv_obj_set_pos(s_pw_ta, 6, 28);
    lv_obj_t *show = lv_checkbox_create(s_pw_scr);
    lv_checkbox_set_text(show, "Show");
    lv_obj_set_pos(show, 236, 38);
    lv_obj_add_event_cb(show, on_show, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *kb = lv_keyboard_create(s_pw_scr);
    lv_obj_set_size(kb, 320, 168);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, s_pw_ta);
    // The keyboard's tick key saves, its keyboard key goes back.
    lv_obj_add_event_cb(kb, on_kb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, on_kb, LV_EVENT_CANCEL, NULL);
    lv_screen_load(s_pw_scr);
}

static void on_join(lv_event_t *e)
{
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_n) return;
    if (s_nets[i].open) save(s_nets[i].ssid, "");
    else ask_password(s_nets[i].ssid);
}

static void on_forget(lv_event_t *e)
{
    // The list button's text is the network name.
    lv_obj_t *row = lv_obj_get_parent(lv_event_get_target_obj(e));
    const char *ssid = lv_list_get_button_text(s_list, row);
    if (!ssid) return;
    char name[33];
    snprintf(name, sizeof(name), "%s", ssid);
    set_status(netcfg_remove(name) == ESP_OK ? "Removed %s" : "Could not remove %s", name);
    fill_later();
}

// ---------------------------------------------------------------- open/close

static void on_close(lv_event_t *e)
{
    lv_timer_delete(s_timer);
    lv_screen_load(s_back);
    lv_obj_delete_async(s_scr);          // we are inside its Done button's event
    s_scr = NULL;                        // a scan still running sees this and stops
}

static void refresh_net(lv_timer_t *t)
{
    char net[64];
    netcfg_summary(net, sizeof(net));
    lv_obj_t *now = lv_obj_get_user_data(s_scr);
    lv_label_set_text(now, net);
}

void wifi_setup_open(lv_obj_t *back)
{
    if (s_scr) return;
    s_back = back;
    s_n = 0;
    s_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_scr);
    lv_label_set_text(title, LV_SYMBOL_WIFI "  WiFi");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(title, 8, 8);
    lv_obj_t *done = lv_button_create(s_scr);
    lv_obj_set_size(done, 70, 34);
    lv_obj_set_pos(done, 244, 4);
    lv_obj_t *l = lv_label_create(done);
    lv_label_set_text(l, "Done");
    lv_obj_center(l);
    lv_obj_add_event_cb(done, on_close, LV_EVENT_CLICKED, NULL);
    s_scan_btn = lv_button_create(s_scr);
    lv_obj_set_size(s_scan_btn, 80, 34);
    lv_obj_set_pos(s_scan_btn, 158, 4);
    l = lv_label_create(s_scan_btn);
    lv_label_set_text(l, LV_SYMBOL_REFRESH " Scan");
    lv_obj_center(l);
    lv_obj_add_event_cb(s_scan_btn, on_scan, LV_EVENT_CLICKED, NULL);

    lv_obj_t *now = lv_label_create(s_scr);
    lv_obj_set_style_text_font(now, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(now, 8, 42);
    lv_obj_set_user_data(s_scr, now);
    s_status = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_status, lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
    lv_obj_set_width(s_status, 304);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_status, 8, 58);

    s_list = lv_list_create(s_scr);
    lv_obj_set_size(s_list, 320, 240 - 76);
    lv_obj_set_pos(s_list, 0, 76);
    lv_obj_set_style_radius(s_list, 0, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);

    refresh_net(NULL);
    s_timer = lv_timer_create(refresh_net, 1000, NULL);
    lv_screen_load(s_scr);
    fill();
    scan();                              // the usual reason for coming here
}
