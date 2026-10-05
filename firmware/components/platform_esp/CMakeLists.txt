idf_component_register(
    SRCS "src/clock.cpp"
         "src/kv_nvs.cpp"
         "src/log.cpp"
         "src/queue_locks.cpp"
    INCLUDE_DIRS "include"
    REQUIRES watch_core
    PRIV_REQUIRES board nvs_flash esp_timer
)
