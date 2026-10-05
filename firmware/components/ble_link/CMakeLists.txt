idf_component_register(
    SRCS "src/ble_link.cpp"
    INCLUDE_DIRS "include"
    REQUIRES bt
    PRIV_REQUIRES watch_core
)
