idf_component_register(
    SRCS "timer_engine.c" "timer_pure.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES freertos nvs_flash port_rtc)
