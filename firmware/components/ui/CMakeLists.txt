# ui コンポーネント: Theme→Component→Screen + firmware 向けポート。
# portable なソースは sources.cmake で sim/ と共有する。
include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

idf_component_register(
    SRCS ${WATCH_UI_SOURCES}
         "src/esp_display.cpp"
         "src/esp_port.cpp"
    INCLUDE_DIRS "include" "src"
    REQUIRES watch_core board
    PRIV_REQUIRES esp_lvgl_port waveshare__esp32_s3_touch_amoled_2_06
                  esp_lcd espressif__esp_lcd_touch lvgl esp_timer esp_system
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -fno-exceptions -fno-rtti)
