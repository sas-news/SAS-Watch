idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash esp_timer esp_lvgl_port
                  board diag ble_link theme_store
                  watch_core platform_esp watch_app ui ota
                  waveshare__esp32_s3_touch_amoled_2_06
)

# アプリの有効/無効 define (app_main.cpp の face hooks ガード用)。
include(${COMPONENT_DIR}/../../apps/cmake/apps.cmake)
target_compile_definitions(${COMPONENT_LIB} PRIVATE ${SAS_APP_DEFINES})
