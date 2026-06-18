idf_component_register(
    SRCS "todo_manager.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash json esp_timer)
