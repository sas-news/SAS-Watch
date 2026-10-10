idf_component_register(
    SRCS "src/watch_app.cpp"
         "src/input.cpp"
         "src/power_apply.cpp"
         "src/ble_glue.cpp"
         "src/imu_service.cpp"
    INCLUDE_DIRS "include" "src"
    REQUIRES watch_core platform_esp board ui ble_link theme_store audio
    PRIV_REQUIRES esp_pm esp_timer lvgl esp_app_format wifi ota
)

# アプリの有効/無効 define (features:: 参照の #if ガード用)。
include(${COMPONENT_DIR}/../../../apps/cmake/apps.cmake)
target_compile_definitions(${COMPONENT_LIB} PUBLIC ${SAS_APP_DEFINES})

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -fno-exceptions -fno-rtti)
