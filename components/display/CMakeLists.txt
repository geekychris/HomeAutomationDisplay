idf_component_register(SRCS "display.c" "dashboard.c" "wifi_setup.c" "ui.c" INCLUDE_DIRS "include"
                       REQUIRES lvgl__lvgl
                       PRIV_REQUIRES esp_driver_gpio nvs_flash netcfg hub board espressif__esp_lvgl_port)
