idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver
    PRIV_REQUIRES epaper_driver_bsp lvgl_port ui i2c_bsp port_shtc3 port_rtc port_touch port_battery wifi_bsp todo_manager wifi_ap todo_web todo_ui)
