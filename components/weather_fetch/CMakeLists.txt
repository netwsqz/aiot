idf_component_register(
    SRCS "weather_fetch.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_http_client esp-tls wifi_bsp nvs_flash json freertos esp_event esp_rom
)
