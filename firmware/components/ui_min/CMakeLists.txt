idf_component_register(
    SRCS "src/ui_min.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES waveshare__esp32_s3_touch_amoled_2_06
                  lvgl__lvgl
                  espressif__esp_lvgl_port
                  esp_timer
)
