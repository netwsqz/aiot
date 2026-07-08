idf_component_register(
    SRCS "weather_cache.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash freertos weather_fetch
)
