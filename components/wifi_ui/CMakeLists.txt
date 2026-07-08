idf_component_register(
    SRCS "wifi_ui.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES lvgl lvgl_port font_cn wifi_ap
)
