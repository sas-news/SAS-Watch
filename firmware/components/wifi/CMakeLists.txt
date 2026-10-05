idf_component_register(
    SRCS "src/wifi.cpp"
    INCLUDE_DIRS "include"
    REQUIRES watch_core
    PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -fno-exceptions -fno-rtti)
