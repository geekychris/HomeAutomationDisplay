# The board layer: panel, touch and backlight for the board chosen in menuconfig.
set(srcs "boards.c" "touch.c")
set(reqs esp_lcd esp_driver_gpio esp_driver_i2c esp_driver_spi nvs_flash
         espressif__esp_lvgl_port espressif__esp_lcd_touch
         espressif__esp_lcd_touch_gt911 espressif__esp_lcd_touch_ft5x06)
if(IDF_TARGET STREQUAL "esp32p4")
    # MIPI DSI panel (board_dsi.c), WiFi through the ESP32-C6 coprocessor.
    list(APPEND srcs "board_dsi.c")
    list(APPEND reqs esp_driver_ldo espressif__esp_lcd_ek79007)
else()
    list(APPEND srcs "board_spi.c")
    list(APPEND reqs espressif__esp_lcd_ili9341 espressif__esp_lcd_st7796)
endif()
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "include"
                       REQUIRES lvgl__lvgl
                       PRIV_REQUIRES ${reqs})
