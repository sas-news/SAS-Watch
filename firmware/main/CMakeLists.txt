idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash esp_timer esp_lvgl_port
                  board diag ble_link theme_store
                  watch_core platform_esp watch_app ui
                  waveshare__esp32_s3_touch_amoled_2_06
)
