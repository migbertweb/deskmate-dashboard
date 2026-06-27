idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "." "include"
                    REQUIRES esp_lcd esp_driver_spi driver
                              esp_event esp_netif esp_wifi nvs_flash
                              lwip esp_http_client json)
