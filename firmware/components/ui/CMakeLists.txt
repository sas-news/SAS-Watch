# ui コンポーネント: Theme→Component→Screen + firmware 向けポート。
# portable なソースは sources.cmake で sim/ と共有する。
include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)
# アプリ (apps/<id>/screen*) の有効/無効とソース収集。
include(${CMAKE_CURRENT_LIST_DIR}/../../../apps/cmake/apps.cmake)

idf_component_register(
    SRCS ${WATCH_UI_SOURCES} ${SAS_APP_UI_SOURCES}
         "src/esp_display.cpp"
         "src/esp_port.cpp"
         "src/lv_mem_psram.c"
         "src/png_stream.cpp"
    INCLUDE_DIRS "include" "src" "${SAS_APP_GEN_DIR}"
    REQUIRES watch_core board
    PRIV_REQUIRES esp_lvgl_port waveshare__esp32_s3_touch_amoled_2_06
                  esp_lcd espressif__esp_lcd_touch lvgl esp_timer esp_system
                  audio ota esp_app_format esp_rom
)

# アプリ側の画面は SAS_APP_<ID> ガード付きの共有コードと同じ define を使う。
target_compile_definitions(${COMPONENT_LIB} PUBLIC ${SAS_APP_DEFINES})
# lv_mem_psram.c の lv_malloc_core 等は liblvgl より後に来ることもある
# リンク順のため、-u で最初から必須シンボル扱いにする。
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_malloc_core")

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -fno-exceptions -fno-rtti)
