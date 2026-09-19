# bmp1bpp.c 由 draw_ui 编译进固件（两份 1bpp BMP 读写共用一份实现），
# 这里只借它的头文件，不重复编译。
idf_component_register(
    SRCS "standby_screen.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES epaper_driver_bsp spiffs log)
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../draw_ui")
