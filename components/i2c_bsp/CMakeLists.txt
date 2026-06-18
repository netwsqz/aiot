idf_component_register(
    SRCS "i2c_bsp.c"
    INCLUDE_DIRS "."
    REQUIRES driver
    PRIV_REQUIRES main)
