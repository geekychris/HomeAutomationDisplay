idf_component_register(SRCS "hub.c" "hub_web.c" INCLUDE_DIRS "include"
                       PRIV_REQUIRES board netcfg nvs_flash json esp_http_server esp_app_format
                                     espressif__esp_websocket_client espressif__mdns)
