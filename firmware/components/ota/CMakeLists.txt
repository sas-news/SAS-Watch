idf_component_register(
    SRCS "src/ota.cpp"
    INCLUDE_DIRS "include"
    REQUIRES watch_core
    PRIV_REQUIRES wifi esp_https_ota esp_http_client app_update mbedtls
                  esp_partition spi_flash joltwallet__littlefs vfs esp_pm
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -fno-exceptions -fno-rtti)
