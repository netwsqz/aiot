idf_component_register(
    SRCS "lvgl_port.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES lvgl esp_timer epaper_driver_bsp driver port_touch)
