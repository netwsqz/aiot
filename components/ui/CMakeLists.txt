idf_component_register(
    SRCS "ui_screen.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES lvgl lvgl_port todo_ui todo_manager font_cn timer_ui timer_engine weather_ui weather_fetch wifi_ui pet_ui draw_ui)
