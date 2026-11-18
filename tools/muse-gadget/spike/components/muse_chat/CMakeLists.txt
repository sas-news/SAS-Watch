# T49 spike: SDK `esp32/components/muse` のうち chat に必要なソースだけを
# 無改変でコンパイルする (muse_chat_session.cpp / muse_chat.c / muse_chat_text.c /
# muse_account_api.c と、muse_chat_text.c が使う muse_text.c)。
#
# CONFIG_SPIKE_WITH_CHAT=n で SRCS を空にすると noise_core だけのビルドになり、
# サイズ差分の基準 (S0) になる。idf_component_register 自体は条件分岐しない
# (ESP-IDF のコンポーネント要件の事前解析が CONFIG を見られないため)。
set(MUSE_CHAT_SRCS "")
if(CONFIG_SPIKE_WITH_CHAT)
    set(MUSE_CHAT_SRCS
        "src/muse_chat.c"
        "src/muse_chat_session.cpp"
        "src/muse_chat_text.c"
        "src/muse_account_api.c"
        "src/muse_text.c")
endif()

idf_component_register(
    SRCS ${MUSE_CHAT_SRCS}
    REQUIRES muse_api noise_core json minimp3_shim esp_http_client esp-tls mbedtls muse_stubs
    PRIV_REQUIRES esp_timer
)

if(MUSE_CHAT_SRCS)
    target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
    target_compile_options(${COMPONENT_LIB} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    )
endif()
