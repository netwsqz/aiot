idf_component_register(
    SRCS "timer_ui.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES timer_engine lvgl font_cn port_rtc)
