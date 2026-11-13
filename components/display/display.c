// The touchscreen: LCD, touch and LVGL setup, and two screens. The main one
// is the dashboard (dashboard.c: the hub's tiles); the gear in its top bar
// opens Setup (WiFi, the hub link, memory, touch calibration).
#include "display.h"
#include "sdkconfig.h"
#include "xpt2046.h"
#include "wifi_setup.h"
#include "dashboard.h"
#include "hub.h"
#include "netcfg.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

static const char *TAG = "display";

// Internal RAM that can be read a byte at a time. Plain MALLOC_CAP_INTERNAL
// on the classic ESP32 includes IRAM, where byte access faults.
#define RAM8 (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

#define W 320
#define H 240
#define LCD_HOST SPI2_HOST
#define BOOT_GPIO 0                 // the BOOT button: hold it to recalibrate touch
// What the screen takes, plus room for WiFi and the web server to work
// afterwards.
#define DISPLAY_RAM_NEEDED (48 * 1024)

#ifdef CONFIG_HAD_LCD_INVERT
#define LCD_INVERT true
#else
#define LCD_INVERT false
#endif
#ifdef CONFIG_HAD_LCD_MIRROR_X
#define LCD_MIRROR_X true
#else
#define LCD_MIRROR_X false
#endif

static lv_display_t *s_disp;
static lv_obj_t *s_main, *s_setup;
static lv_obj_t *s_net, *s_hub, *s_heap;

// ---------------------------------------------------------------- actions

static void on_wifi(lv_event_t *e) { wifi_setup_open(s_setup); }
static void open_setup(void) { lv_screen_load(s_setup); }
static void on_back(lv_event_t *e) { lv_screen_load(s_main); }

static void calibrate_begin(void);
static void on_calibrate(lv_event_t *e) { calibrate_begin(); }

// ---------------------------------------------------------------- refresh

static void tick(lv_timer_t *t)
{
    if (lv_screen_active() != s_setup) return;      // only Setup shows these
    char line[96];
    netcfg_summary(line, sizeof(line));
    lv_label_set_text(s_net, line);
    char hub[96];
    hub_status_text(hub, sizeof(hub));
    lv_label_set_text_fmt(s_hub, "%s\nThis display is %s - find it in the hub's admin page.", hub, hub_device_id());
    lv_label_set_text_fmt(s_heap, "free memory %u KB (lowest %u KB)",
                          (unsigned)(heap_caps_get_free_size(RAM8) / 1024),
                          (unsigned)(heap_caps_get_minimum_free_size(RAM8) / 1024));
}

// ---------------------------------------------------------------- building the screens

static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *arg)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, arg);
    return b;
}

static lv_obj_t *small(lv_obj_t *parent)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(l, lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
    return l;
}

static void build_setup(lv_obj_t *scr)
{
    lv_obj_t *top = lv_obj_create(scr);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, lv_pct(100), 36);
    lv_obj_t *back = button(top, LV_SYMBOL_LEFT "  Back", on_back, NULL);
    lv_obj_set_height(back, 32);
    lv_obj_t *title = lv_label_create(top);
    lv_label_set_text(title, "Setup");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_RIGHT_MID, -4, 0);

    lv_obj_t *col = lv_obj_create(scr);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, lv_pct(100), 240 - 52);
    lv_obj_set_pos(col, 0, 40);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 6, 0);
    lv_obj_t *net = lv_obj_create(col);
    lv_obj_remove_style_all(net);
    lv_obj_set_size(net, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(net, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(net, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_net = lv_label_create(net);
    lv_label_set_long_mode(s_net, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(s_net, 1);
    lv_obj_t *wifi = button(net, LV_SYMBOL_WIFI "  WiFi", on_wifi, NULL);
    lv_obj_set_height(wifi, 38);
    s_hub = small(col);
    lv_obj_set_width(s_hub, lv_pct(100));
    lv_label_set_long_mode(s_hub, LV_LABEL_LONG_WRAP);
    s_heap = small(col);
    lv_obj_t *b = button(col, "Calibrate touch", on_calibrate, NULL);
    lv_obj_set_height(b, 38);
    lv_obj_t *t = small(col);
    lv_label_set_text(t, "Holding the BOOT button also starts touch calibration. "
                         "The hub address can be set at http://<name>.local/hub.\n"
                         "(c) 2026 Chris Collins - MIT License");
    lv_obj_set_width(t, lv_pct(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
}

// ---------------------------------------------------------------- touch calibration
//
// A crosshair at three points in turn; hold each until it turns green. While
// this runs the touch input reports nothing, so taps cannot reach the UI.
static const int CAL_X[3] = { 30, W - 30, W / 2 }, CAL_Y[3] = { 30, H / 2, H - 30 };
static lv_obj_t   *s_cal_scr, *s_cal_h, *s_cal_v, *s_cal_txt;
static lv_timer_t *s_cal_timer;
static volatile bool s_calibrating;
static int s_cal_i, s_cal_held, s_cal_free, s_cal_rx[3], s_cal_ry[3];
static long s_cal_sx, s_cal_sy;

static void cal_target(void)
{
    const lv_color_t c = lv_color_white();
    lv_obj_set_style_bg_color(s_cal_h, c, 0);
    lv_obj_set_style_bg_color(s_cal_v, c, 0);
    lv_obj_set_pos(s_cal_h, CAL_X[s_cal_i] - 12, CAL_Y[s_cal_i] - 1);
    lv_obj_set_pos(s_cal_v, CAL_X[s_cal_i] - 1, CAL_Y[s_cal_i] - 12);
    s_cal_held = s_cal_free = 0;
    s_cal_sx = s_cal_sy = 0;
}

static void cal_tick(lv_timer_t *t)
{
    int rx, ry;
    const bool down = xpt_read_raw(&rx, &ry);
    if (s_cal_held < 12) {                       // collecting: hold still on the cross
        if (!down) { s_cal_held = 0; s_cal_sx = s_cal_sy = 0; return; }
        if (++s_cal_held > 4) { s_cal_sx += rx; s_cal_sy += ry; }   // skip the first, while pressing
        if (s_cal_held == 12) {
            s_cal_rx[s_cal_i] = s_cal_sx / 8;
            s_cal_ry[s_cal_i] = s_cal_sy / 8;
            lv_obj_set_style_bg_color(s_cal_h, lv_palette_main(LV_PALETTE_GREEN), 0);
            lv_obj_set_style_bg_color(s_cal_v, lv_palette_main(LV_PALETTE_GREEN), 0);
        }
        return;
    }
    if (down) { s_cal_free = 0; return; }        // done here: wait for the finger to lift
    if (++s_cal_free < 4) return;
    ESP_LOGI(TAG, "cal point %d: screen %d,%d raw %d,%d", s_cal_i, CAL_X[s_cal_i], CAL_Y[s_cal_i],
             s_cal_rx[s_cal_i], s_cal_ry[s_cal_i]);
    if (++s_cal_i < 3) { cal_target(); return; }
    if (!xpt_set_calibration(CAL_X, CAL_Y, s_cal_rx, s_cal_ry)) {
        lv_label_set_text(s_cal_txt, "That didn't line up - again, please.\nTap and hold each cross.");
        s_cal_i = 0;
        cal_target();
        return;
    }
    lv_timer_delete(s_cal_timer);
    s_cal_timer = NULL;
    lv_screen_load(s_main);
    lv_obj_delete(s_cal_scr);
    s_cal_scr = NULL;
    s_calibrating = false;
}

static void calibrate_begin(void)
{
    if (s_cal_scr) return;
    s_calibrating = true;
    s_cal_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_cal_scr, lv_color_black(), 0);
    s_cal_txt = lv_label_create(s_cal_scr);
    lv_label_set_text(s_cal_txt, "Touch calibration\nTap and hold each cross until it turns green.");
    lv_obj_set_style_text_align(s_cal_txt, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_cal_txt, LV_ALIGN_CENTER, 0, -20);
    s_cal_h = lv_obj_create(s_cal_scr);
    s_cal_v = lv_obj_create(s_cal_scr);
    lv_obj_t *bars[2] = { s_cal_h, s_cal_v };
    for (int i = 0; i < 2; i++) {
        lv_obj_remove_style_all(bars[i]);
        lv_obj_set_style_bg_opa(bars[i], LV_OPA_COVER, 0);
    }
    lv_obj_set_size(s_cal_h, 25, 3);
    lv_obj_set_size(s_cal_v, 3, 25);
    s_cal_i = 0;
    cal_target();
    lv_screen_load(s_cal_scr);
    s_cal_timer = lv_timer_create(cal_tick, 30, NULL);
}

static void boot_button(lv_timer_t *t)
{
    static int held;
    held = gpio_get_level(BOOT_GPIO) ? 0 : held + 1;
    if (held == 2) calibrate_begin();
}

static void touch_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    int x, y;
    if (s_calibrating || !xpt_read(&x, &y)) { data->state = LV_INDEV_STATE_RELEASED; return; }
    data->point.x = x < 0 ? 0 : x >= W ? W - 1 : x;
    data->point.y = y < 0 ? 0 : y >= H ? H - 1 : y;
    data->state = LV_INDEV_STATE_PRESSED;
}

// ---------------------------------------------------------------- start

static esp_err_t lcd_init(esp_lcd_panel_io_handle_t *io, esp_lcd_panel_handle_t *panel)
{
    const spi_bus_config_t bus = {
        .sclk_io_num = CONFIG_HAD_LCD_SCLK_GPIO,
        .mosi_io_num = CONFIG_HAD_LCD_MOSI_GPIO,
        .miso_io_num = -1,                   // never read; GPIO 12 is a strapping pin
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = W * 10 * 2,
    };
    esp_err_t err = spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;
    const esp_lcd_panel_io_spi_config_t ioc = {
        .dc_gpio_num = CONFIG_HAD_LCD_DC_GPIO,
        .cs_gpio_num = CONFIG_HAD_LCD_CS_GPIO,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    if ((err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &ioc, io)) != ESP_OK) return err;
    const esp_lcd_panel_dev_config_t pc = {
        .reset_gpio_num = -1,                // tied to EN: reset with the chip
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    if ((err = esp_lcd_new_panel_ili9341(*io, &pc, panel)) != ESP_OK) return err;
    esp_lcd_panel_reset(*panel);
    esp_lcd_panel_init(*panel);
    // Some CYD batches have a panel that needs inverting to show the right colours.
    esp_lcd_panel_invert_color(*panel, LCD_INVERT);
    esp_lcd_panel_disp_on_off(*panel, true);
    return ESP_OK;
}

esp_err_t display_start(void)
{
    ESP_LOGI(TAG, "starting: %u KB internal RAM free, largest block %u KB",
             (unsigned)(heap_caps_get_free_size(RAM8) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(RAM8) / 1024));
    // LVGL stops dead in an endless loop when an allocation fails. Better no
    // screen than a hung device.
    if (heap_caps_get_free_size(RAM8) < DISPLAY_RAM_NEEDED) {
        ESP_LOGE(TAG, "not starting: needs ~%u KB of RAM and there is not that much free",
                 DISPLAY_RAM_NEEDED / 1024);
        return ESP_ERR_NO_MEM;
    }
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    esp_err_t err = lcd_init(&io, &panel);
    if (err != ESP_OK) { ESP_LOGE(TAG, "LCD: %s", esp_err_to_name(err)); return err; }

    // Below the web server (5). Core 1, away from WiFi.
    lvgl_port_cfg_t lc = ESP_LVGL_PORT_INIT_CONFIG();
    lc.task_priority = 3;
    lc.task_stack = 6144;
    lc.task_affinity = 1;
    if ((err = lvgl_port_init(&lc)) != ESP_OK) { ESP_LOGE(TAG, "LVGL: %s", esp_err_to_name(err)); return err; }
    const lvgl_port_display_cfg_t dc = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = W * 10,
        .double_buffer = true,
        .hres = W,
        .vres = H,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = { .swap_xy = true, .mirror_x = LCD_MIRROR_X, .mirror_y = false },
        .flags = { .buff_dma = true, .swap_bytes = true },
    };
    s_disp = lvgl_port_add_disp(&dc);
    if (!s_disp) { ESP_LOGE(TAG, "LVGL display: no memory for draw buffers?"); return ESP_FAIL; }

    gpio_config_t bl = { .pin_bit_mask = 1ULL << CONFIG_HAD_LCD_BL_GPIO, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&bl);
    gpio_config_t boot = { .pin_bit_mask = 1ULL << BOOT_GPIO, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&boot);
    xpt_init();

    lvgl_port_lock(0);
    lv_display_set_theme(s_disp, lv_theme_default_init(s_disp, lv_palette_main(LV_PALETTE_AMBER),
                                                       lv_palette_main(LV_PALETTE_BLUE_GREY), true,
                                                       &lv_font_montserrat_14));
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_cb);
    lv_indev_set_display(indev, s_disp);

    s_main = lv_screen_active();
    dashboard_build(s_main, open_setup);
    s_setup = lv_obj_create(NULL);
    lv_obj_set_style_pad_all(s_setup, 8, 0);
    build_setup(s_setup);
    lv_timer_create(tick, 1000, NULL);
    lv_timer_create(boot_button, 50, NULL);
    if (!xpt_calibrated()) calibrate_begin();
    lvgl_port_unlock();

    gpio_set_level(CONFIG_HAD_LCD_BL_GPIO, 1);
    ESP_LOGI(TAG, "touchscreen up, %u KB internal RAM free",
             (unsigned)(heap_caps_get_free_size(RAM8) / 1024));
    return ESP_OK;
}
