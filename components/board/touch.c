// Touch: an XPT2046 resistive controller (with a 3-point calibration kept in
// NVS), or a GT911 / FT6336 capacitive one through esp_lcd_touch.
//
// The XPT2046 either has pins of its own, bit-banged (the 2.8" board, which
// leaves SPI3 for the microSD slot), or is a second device on the LCD's SPI
// bus. A few microseconds per sample, at most every 30 ms, costs nothing.
#include "board_def.h"
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "nvs.h"

static const char *TAG = "touch";

esp_err_t board_spi_bus(void);      // board_spi.c (absent on boards without SPI panels)

// ---------------------------------------------------------------- XPT2046

// Command bytes: start bit, channel, 12-bit differential mode, and PD = 00 so
// the pen interrupt stays enabled between conversions.
#define CMD_X  0xD0
#define CMD_Y  0x90
#define CMD_Z1 0xB0
#define CMD_Z2 0xC0
#define Z_MIN  300                  // below this the plate is barely touched and positions are noise

static spi_device_handle_t s_spi;   // XPT2046_SPI
static float s_cal[6];              // screen = [a b c; d e f] * (raw x, raw y, 1)
static bool  s_have_cal;

static uint16_t xfer_gpio(uint8_t cmd)
{
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(BOARD.t_mosi, (cmd >> i) & 1);
        esp_rom_delay_us(1);
        gpio_set_level(BOARD.t_sclk, 1);
        esp_rom_delay_us(1);
        gpio_set_level(BOARD.t_sclk, 0);
    }
    gpio_set_level(BOARD.t_mosi, 0);
    // One busy clock, then the 12 result bits, MSB first, then padding.
    uint16_t v = 0;
    for (int i = 0; i < 16; i++) {
        gpio_set_level(BOARD.t_sclk, 1);
        esp_rom_delay_us(1);
        v = (v << 1) | gpio_get_level(BOARD.t_miso);
        gpio_set_level(BOARD.t_sclk, 0);
        esp_rom_delay_us(1);
    }
    return (v >> 3) & 0x0FFF;
}

static uint16_t xfer_spi(uint8_t cmd)
{
    spi_transaction_t t = {
        .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
        .length = 24,
        .tx_data = { cmd, 0, 0 },
    };
    if (spi_device_polling_transmit(s_spi, &t) != ESP_OK) return 0;
    return (((uint16_t)t.rx_data[1] << 8) | t.rx_data[2]) >> 3 & 0x0FFF;
}

static uint16_t xfer(uint8_t cmd) { return s_spi ? xfer_spi(cmd) : xfer_gpio(cmd); }

static void load_cal(void)
{
    nvs_handle_t h;
    if (nvs_open("touch", NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof(s_cal);
    s_have_cal = nvs_get_blob(h, "cal", s_cal, &n) == ESP_OK && n == sizeof(s_cal);
    nvs_close(h);
}

static esp_err_t xpt_init(void)
{
    if (BOARD.touch == TOUCH_XPT2046_SPI) {
        esp_err_t err = board_spi_bus();
        if (err != ESP_OK) return err;
        const spi_device_interface_config_t dev = {
            .clock_speed_hz = 2 * 1000 * 1000,
            .mode = 0,
            .spics_io_num = BOARD.t_cs,
            .queue_size = 1,
        };
        if ((err = spi_bus_add_device(SPI2_HOST, &dev, &s_spi)) != ESP_OK) return err;
    } else {
        gpio_config_t out = {
            .pin_bit_mask = (1ULL << BOARD.t_sclk) | (1ULL << BOARD.t_mosi) | (1ULL << BOARD.t_cs),
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&out);
        gpio_config_t in = { .pin_bit_mask = 1ULL << BOARD.t_miso, .mode = GPIO_MODE_INPUT };
        gpio_config(&in);
        gpio_set_level(BOARD.t_cs, 1);
        gpio_set_level(BOARD.t_sclk, 0);
    }
    if (BOARD.t_irq >= 0) {
        // Input-only pins (34-39) have no internal pulls; the boards pull the pen interrupt up.
        gpio_config_t irq = { .pin_bit_mask = 1ULL << BOARD.t_irq, .mode = GPIO_MODE_INPUT };
        gpio_config(&irq);
    }
    load_cal();
    ESP_LOGI(TAG, "XPT2046 (%s) ready, %s", s_spi ? "shared SPI bus" : "own pins",
             s_have_cal ? "calibrated" : "not calibrated yet");
    return ESP_OK;
}

static bool xpt_read_raw(int *rx, int *ry)
{
    if (BOARD.t_irq >= 0 && gpio_get_level(BOARD.t_irq)) return false;
    if (!s_spi) gpio_set_level(BOARD.t_cs, 0);
    const int z = xfer(CMD_Z1) + 4095 - xfer(CMD_Z2);
    bool ok = z >= Z_MIN;
    if (ok) {
        // Several readings, keep the middle of them: the first after the
        // plates switch is often off, and a resistive panel is noisy anyway.
        int xs[5], ys[5];
        xfer(CMD_X);
        for (int i = 0; i < 5; i++) { xs[i] = xfer(CMD_X); ys[i] = xfer(CMD_Y); }
        for (int i = 1; i < 5; i++)
            for (int j = i; j > 0 && xs[j] < xs[j - 1]; j--) { int t = xs[j]; xs[j] = xs[j - 1]; xs[j - 1] = t; }
        for (int i = 1; i < 5; i++)
            for (int j = i; j > 0 && ys[j] < ys[j - 1]; j--) { int t = ys[j]; ys[j] = ys[j - 1]; ys[j - 1] = t; }
        *rx = (xs[1] + xs[2] + xs[3]) / 3;
        *ry = (ys[1] + ys[2] + ys[3]) / 3;
    }
    if (!s_spi) gpio_set_level(BOARD.t_cs, 1);   // PD = 00 in the last command left the interrupt armed
    return ok;
}

// ---------------------------------------------------------------- capacitive

static esp_lcd_touch_handle_t s_tp;

static esp_err_t cap_init(void)
{
    i2c_master_bus_handle_t bus;
    const i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD.t_sda,
        .scl_io_num = BOARD.t_scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bc, &bus);
    if (err != ESP_OK) return err;

    esp_lcd_panel_io_i2c_config_t ioc;
    if (BOARD.touch == TOUCH_GT911) {
        ioc = (esp_lcd_panel_io_i2c_config_t)ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
        // The GT911 answers at 0x5D or 0x14, depending on its INT pin at
        // reset - and on some boards INT is not wired as documented. Ask.
        if (i2c_master_probe(bus, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, 50) != ESP_OK &&
            i2c_master_probe(bus, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP, 50) == ESP_OK)
            ioc.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
    } else {
        ioc = (esp_lcd_panel_io_i2c_config_t)ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    }
    ioc.scl_speed_hz = 400 * 1000;
    esp_lcd_panel_io_handle_t io;
    if ((err = esp_lcd_new_panel_io_i2c(bus, &ioc, &io)) != ESP_OK) return err;

    const esp_lcd_touch_config_t tc = {
        .x_max = BOARD.t_x_max,
        .y_max = BOARD.t_y_max,
        .rst_gpio_num = BOARD.t_rst,
        .int_gpio_num = BOARD.t_int,
        .flags = { .swap_xy = BOARD.t_swap_xy, .mirror_x = BOARD.t_mirror_x, .mirror_y = BOARD.t_mirror_y },
    };
    err = BOARD.touch == TOUCH_GT911 ? esp_lcd_touch_new_i2c_gt911(io, &tc, &s_tp)
                                     : esp_lcd_touch_new_i2c_ft5x06(io, &tc, &s_tp);
    if (err == ESP_OK) ESP_LOGI(TAG, "%s ready at 0x%02x", BOARD.touch == TOUCH_GT911 ? "GT911" : "FT6336",
                                (unsigned)ioc.dev_addr);
    return err;
}

static bool cap_read(int *x, int *y)
{
    if (!s_tp || esp_lcd_touch_read_data(s_tp) != ESP_OK) return false;
    esp_lcd_touch_point_data_t pt[1];
    uint8_t n = 0;
    if (esp_lcd_touch_get_data(s_tp, pt, &n, 1) != ESP_OK || !n) return false;
    *x = pt[0].x;
    *y = pt[0].y;
    return true;
}

// ---------------------------------------------------------------- the interface

bool board_touch_resistive(void) { return BOARD.touch == TOUCH_XPT2046_GPIO || BOARD.touch == TOUCH_XPT2046_SPI; }

esp_err_t board_touch_init(void)
{
    const esp_err_t err = board_touch_resistive() ? xpt_init() : cap_init();
    if (err != ESP_OK) ESP_LOGE(TAG, "touch did not start: %s", esp_err_to_name(err));
    return err;
}

bool board_touch_calibrated(void) { return !board_touch_resistive() || s_have_cal; }

bool board_touch_read_raw(int *rx, int *ry) { return board_touch_resistive() && xpt_read_raw(rx, ry); }

bool board_touch_read(int *x, int *y)
{
    if (!board_touch_resistive()) return cap_read(x, y);
    int rx, ry;
    if (!s_have_cal || !xpt_read_raw(&rx, &ry)) return false;
    *x = (int)(s_cal[0] * rx + s_cal[1] * ry + s_cal[2]);
    *y = (int)(s_cal[3] * rx + s_cal[4] * ry + s_cal[5]);
    return true;
}

// Three taps fix an affine map, which also absorbs however the panel happens
// to be rotated or mirrored relative to the display.
bool board_touch_set_calibration(const int sx[3], const int sy[3], const int rx[3], const int ry[3])
{
    const float det = (float)rx[0] * (ry[1] - ry[2]) - (float)ry[0] * (rx[1] - rx[2])
                    + ((float)rx[1] * ry[2] - (float)rx[2] * ry[1]);
    if (det > -1000.0f && det < 1000.0f) return false;        // taps too close or in a line
    float c[6];
    for (int k = 0; k < 2; k++) {
        const int *s = k ? sy : sx;
        c[k * 3 + 0] = ((float)s[0] * (ry[1] - ry[2]) - (float)ry[0] * (s[1] - s[2]) + ((float)s[1] * ry[2] - (float)s[2] * ry[1])) / det;
        c[k * 3 + 1] = ((float)rx[0] * (s[1] - s[2]) - (float)s[0] * (rx[1] - rx[2]) + ((float)rx[1] * s[2] - (float)rx[2] * s[1])) / det;
        c[k * 3 + 2] = ((float)rx[0] * ((float)ry[1] * s[2] - (float)ry[2] * s[1])
                      - (float)ry[0] * ((float)rx[1] * s[2] - (float)rx[2] * s[1])
                      + (float)s[0] * ((float)rx[1] * ry[2] - (float)rx[2] * ry[1])) / det;
    }
    memcpy(s_cal, c, sizeof(s_cal));
    s_have_cal = true;
    nvs_handle_t h;
    if (nvs_open("touch", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "cal", s_cal, sizeof(s_cal));
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "calibrated: x = %.4f rx + %.4f ry + %.1f, y = %.4f rx + %.4f ry + %.1f",
             c[0], c[1], c[2], c[3], c[4], c[5]);
    return true;
}
