// XPT2046 resistive touch controller, bit-banged. On the Cheap Yellow Display
// it has its own pins, not the display's SPI bus, and leaving SPI3 free keeps
// it for the microSD slot or anything else. A few microseconds of GPIO
// wiggling per sample, at most every 30 ms, costs nothing.
#include "xpt2046.h"
#include "sdkconfig.h"
#include <string.h>
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "touch";

#define SCLK CONFIG_HAD_TOUCH_SCLK_GPIO
#define MOSI CONFIG_HAD_TOUCH_MOSI_GPIO
#define MISO CONFIG_HAD_TOUCH_MISO_GPIO
#define CS   CONFIG_HAD_TOUCH_CS_GPIO
#define IRQ  CONFIG_HAD_TOUCH_IRQ_GPIO

// Command bytes: start bit, channel, 12-bit differential mode, and PD = 00 so
// the pen interrupt stays enabled between conversions.
#define CMD_X  0xD0
#define CMD_Y  0x90
#define CMD_Z1 0xB0
#define CMD_Z2 0xC0

// Below this the plate is barely touched and positions are noise.
#define Z_MIN  300

static float s_cal[6];           // screen = [a b c; d e f] * (raw x, raw y, 1)
static bool  s_have_cal;

static uint16_t xfer(uint8_t cmd)
{
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(MOSI, (cmd >> i) & 1);
        esp_rom_delay_us(1);
        gpio_set_level(SCLK, 1);
        esp_rom_delay_us(1);
        gpio_set_level(SCLK, 0);
    }
    gpio_set_level(MOSI, 0);
    // One busy clock, then the 12 result bits, MSB first, then padding.
    uint16_t v = 0;
    for (int i = 0; i < 16; i++) {
        gpio_set_level(SCLK, 1);
        esp_rom_delay_us(1);
        v = (v << 1) | gpio_get_level(MISO);
        gpio_set_level(SCLK, 0);
        esp_rom_delay_us(1);
    }
    return (v >> 3) & 0x0FFF;
}

static void load_cal(void)
{
    nvs_handle_t h;
    if (nvs_open("touch", NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof(s_cal);
    s_have_cal = nvs_get_blob(h, "cal", s_cal, &n) == ESP_OK && n == sizeof(s_cal);
    nvs_close(h);
}

void xpt_init(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << SCLK) | (1ULL << MOSI) | (1ULL << CS),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    // 36 and 39 are input-only and have no internal pulls; the board has a
    // pull-up on the pen interrupt.
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << MISO) | (1ULL << IRQ),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&in);
    gpio_set_level(CS, 1);
    gpio_set_level(SCLK, 0);
    load_cal();
    ESP_LOGI(TAG, "XPT2046 ready, %s", s_have_cal ? "calibrated" : "not calibrated yet");
}

bool xpt_read_raw(int *rx, int *ry)
{
    if (gpio_get_level(IRQ)) return false;
    gpio_set_level(CS, 0);
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
    gpio_set_level(CS, 1);         // PD = 00 in the last command left the interrupt armed
    return ok;
}

bool xpt_calibrated(void) { return s_have_cal; }

bool xpt_read(int *x, int *y)
{
    int rx, ry;
    if (!s_have_cal || !xpt_read_raw(&rx, &ry)) return false;
    *x = (int)(s_cal[0] * rx + s_cal[1] * ry + s_cal[2]);
    *y = (int)(s_cal[3] * rx + s_cal[4] * ry + s_cal[5]);
    return true;
}

// Three taps fix an affine map, which also absorbs however the panel happens
// to be rotated or mirrored relative to the display.
bool xpt_set_calibration(const int sx[3], const int sy[3], const int rx[3], const int ry[3])
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

