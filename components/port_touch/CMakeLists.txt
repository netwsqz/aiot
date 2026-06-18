idf_component_register(
    SRCS "port_touch.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES main i2c_bsp driver)
