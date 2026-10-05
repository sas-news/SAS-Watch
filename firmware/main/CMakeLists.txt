idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash esp_timer
                  board ui_min diag
                  waveshare__esp32_s3_touch_amoled_2_06
)
