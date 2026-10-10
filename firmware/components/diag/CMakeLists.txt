idf_component_register(
    SRCS "src/diag.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES waveshare__esp32_s3_touch_amoled_2_06
                  board
                  esp_driver_i2c
                  esp_hw_support
                  esp_psram
                  spi_flash
                  esp_partition
)
