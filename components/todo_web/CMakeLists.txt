idf_component_register(
    SRCS "todo_web.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_http_server todo_manager json standby_screen wifi_bsp)
