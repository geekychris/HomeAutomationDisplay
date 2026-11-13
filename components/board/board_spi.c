// Panels on SPI (the classic-ESP32 boards): ILI9341 and ST7796.
#include "board_def.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_st7796.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "board";

#define LCD_HOST SPI2_HOST
#define LINES 10                    // draw buffer height: two of these, in internal RAM

#ifdef CONFIG_HAD_LCD_INVERT
#define FLIP_INVERT true
#else
#define FLIP_INVERT false
#endif
#ifdef CONFIG_HAD_LCD_MIRROR_X
#define FLIP_MIRROR_X true
#else
#define FLIP_MIRROR_X false
#endif

const board_info_t *board(void) { return &BOARD.info; }

// The SPI bus, shared with the touch controller on boards where it sits on it.
esp_err_t board_spi_bus(void)
{
    static bool up;
    if (up) return ESP_OK;
    const spi_bus_config_t bus = {
        .sclk_io_num = BOARD.sclk,
        .mosi_io_num = BOARD.mosi,
        // Only read when touch shares the bus. On the 2.8" board GPIO 12 is
        // MISO, and a strapping pin best left alone.
        .miso_io_num = BOARD.touch == TOUCH_XPT2046_SPI ? BOARD.miso : -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD.info.w * LINES * 2,
    };
    esp_err_t err = spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO);
    up = err == ESP_OK;
    return err;
}

static esp_err_t lcd_init(esp_lcd_panel_io_handle_t *io, esp_lcd_panel_handle_t *panel)
{
    esp_err_t err = board_spi_bus();
    if (err != ESP_OK) return err;
    const esp_lcd_panel_io_spi_config_t ioc = {
        .dc_gpio_num = BOARD.dc,
        .cs_gpio_num = BOARD.cs,
        .pclk_hz = BOARD.pclk_mhz * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    if ((err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &ioc, io)) != ESP_OK) return err;
    const esp_lcd_panel_dev_config_t pc = {
        .reset_gpio_num = BOARD.rst,        // -1 where it is tied to EN
        .rgb_ele_order = BOARD.bgr ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    switch (BOARD.lcd) {
    case LCD_ILI9341: err = esp_lcd_new_panel_ili9341(*io, &pc, panel); break;
    case LCD_ST7796:  err = esp_lcd_new_panel_st7796(*io, &pc, panel); break;
    default:          err = ESP_ERR_NOT_SUPPORTED; break;
    }
    if (err != ESP_OK) return err;
    esp_lcd_panel_reset(*panel);
    esp_lcd_panel_init(*panel);
    // Batches of the same board differ: the menuconfig options flip the board's default.
    esp_lcd_panel_invert_color(*panel, BOARD.invert != FLIP_INVERT);
    esp_lcd_panel_disp_on_off(*panel, true);
    return ESP_OK;
}

lv_display_t *board_display_start(void)
{
    gpio_config_t bl = { .pin_bit_mask = 1ULL << BOARD.bl, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&bl);
    board_backlight(false);

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    esp_err_t err = lcd_init(&io, &panel);
    if (err != ESP_OK) { ESP_LOGE(TAG, "LCD: %s", esp_err_to_name(err)); return NULL; }
    const lvgl_port_display_cfg_t dc = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = BOARD.info.w * LINES,
        .double_buffer = true,
        .hres = BOARD.info.w,
        .vres = BOARD.info.h,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = { .swap_xy = BOARD.swap_xy, .mirror_x = BOARD.mirror_x != FLIP_MIRROR_X,
                      .mirror_y = BOARD.mirror_y },
        .flags = { .buff_dma = true, .swap_bytes = true },
    };
    lv_display_t *d = lvgl_port_add_disp(&dc);
    if (!d) ESP_LOGE(TAG, "LVGL display: no memory for draw buffers?");
    return d;
}

void board_backlight(bool on) { gpio_set_level(BOARD.bl, on != BOARD.bl_low); }
