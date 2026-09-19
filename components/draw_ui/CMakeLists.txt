idf_component_register(
    SRCS "draw_ui.c" "bmp1bpp.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES lvgl spiffs log heap)
