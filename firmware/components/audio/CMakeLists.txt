idf_component_register(
    SRCS "src/audio.cpp"
    INCLUDE_DIRS "include"
    REQUIRES watch_core board
    PRIV_REQUIRES waveshare__esp32_s3_touch_amoled_2_06 esp_codec_dev
                  joltwallet__littlefs driver esp_timer esp_pm
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -fno-exceptions -fno-rtti)
