idf_component_register(
    SRCS "pet_ui.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES lvgl todo_manager timer_engine port_rtc port_battery)
