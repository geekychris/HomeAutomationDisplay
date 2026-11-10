#include "nvs_flash.h"
#include "esp_log.h"
#include "netcfg.h"
#include "display.h"
#include "hub.h"

static const char *TAG = "had";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // WiFi first: it wants its buffers while the heap is still whole, and
    // the setup page stays reachable even if the screen does not start.
    netcfg_start();
    // Before the screen: the dashboard reads the hub's model from its first frame.
    if (hub_start() != ESP_OK) ESP_LOGE(TAG, "hub link failed to start");
    if (display_start() != ESP_OK) ESP_LOGE(TAG, "touchscreen failed to start");
    ESP_LOGI(TAG, "ready");
}
