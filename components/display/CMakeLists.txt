idf_component_register(SRCS "display.c" "dashboard.c" "xpt2046.c" "wifi_setup.c" INCLUDE_DIRS "include"
                       REQUIRES lvgl__lvgl
                       PRIV_REQUIRES esp_lcd esp_driver_spi esp_driver_gpio nvs_flash netcfg hub
                                     espressif__esp_lvgl_port espressif__esp_lcd_ili9341)
