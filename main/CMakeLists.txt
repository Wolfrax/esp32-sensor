idf_component_register(
    SRCS "i2c_reader.c" "main.c"  # "i2c_scanner.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES mdns esp_wifi esp_event esp_netif nvs_flash esp_http_server fatfs esp_driver_i2c
)