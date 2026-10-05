idf_component_register(
    SRCS "src/board.cpp"
         "src/buttons.cpp"
         "src/pmic.cpp"
         "src/rtc.cpp"
         "src/imu.cpp"
         "src/haptics.cpp"
         "src/sleep.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES waveshare__esp32_s3_touch_amoled_2_06
                  waveshare__qmi8658
                  waveshare__pcf85063a
                  espressif__button
                  XPowersLib
                  esp_driver_gpio
                  esp_driver_i2c
                  esp_timer
                  esp_hw_support
)
