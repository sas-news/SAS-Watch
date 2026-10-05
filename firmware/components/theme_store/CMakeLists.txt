idf_component_register(
    SRCS "src/theme_store.cpp"
    INCLUDE_DIRS "include"
    REQUIRES watch_core
    PRIV_REQUIRES vfs joltwallet__littlefs
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -fno-exceptions -fno-rtti)
