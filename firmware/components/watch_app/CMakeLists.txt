# MusePort アダプタは CONFIG_WATCH_ENABLE_MUSE_LINK=y のときだけコンパイルする
# (既定 off では 0B。muse_link は off だとヘッダのみのコンポーネント)。
set(WATCH_APP_SRCS
    "src/watch_app.cpp"
    "src/input.cpp"
    "src/power_apply.cpp"
    "src/ble_glue.cpp"
    "src/imu_service.cpp")
if(CONFIG_WATCH_ENABLE_MUSE_LINK)
    list(APPEND WATCH_APP_SRCS "src/muse_port.cpp")
endif()

idf_component_register(
    SRCS ${WATCH_APP_SRCS}
    INCLUDE_DIRS "include" "src"
    REQUIRES watch_core platform_esp board ui ble_link theme_store audio
    PRIV_REQUIRES esp_pm esp_timer lvgl esp_app_format wifi ota esp_driver_gpio
                 muse_link
)

# アプリの有効/無効 define (features:: 参照の #if ガード用)。
include(${COMPONENT_DIR}/../../../apps/cmake/apps.cmake)
target_compile_definitions(${COMPONENT_LIB} PUBLIC ${SAS_APP_DEFINES})

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -fno-exceptions -fno-rtti)
