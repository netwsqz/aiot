idf_component_register(
    SRCS "todo_ui.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES todo_manager lvgl epaper_driver_bsp font_cn)
