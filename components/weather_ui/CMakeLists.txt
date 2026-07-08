idf_component_register(
    SRCS "weather_ui.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES lvgl lvgl_port weather_fetch weather_cache font_cn
)
