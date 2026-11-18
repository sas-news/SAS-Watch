# Muse Gadget SDK の chat/Noise 層 (T50 Phase 1)
#
# 既定は無効。CONFIG_WATCH_ENABLE_MUSE_LINK=n のときは SRCS が空になり、
# このコンポーネントはヘッダだけの存在になる (バイナリへの追加 0B)。
#
# 有効時のみ SDK コピー (Apache-2.0) とアダプタをコンパイルする。
# ソース一覧と sha256 は README.md、実測サイズは docs/muse-bmin.md を参照。
# idf_component_register 自体は Kconfig で条件分岐しない (ESP-IDF の
# コンポーネント要件の事前解析が CONFIG を参照できないため)。

set(MUSE_LINK_SRCS "")
if(CONFIG_WATCH_ENABLE_MUSE_LINK)
    # SAS-Watch 側のアダプタ (NVS/ワット/字幕/選択肢の差し替え)
    list(APPEND MUSE_LINK_SRCS
        "src/muse_link.cpp"
        "src/muse_adapters.cpp")
    # SDK の chat 層 (無改変)
    list(APPEND MUSE_LINK_SRCS
        "sdk/chat/src/muse_chat.c"
        "sdk/chat/src/muse_chat_session.cpp"
        "sdk/chat/src/muse_chat_text.c"
        "sdk/chat/src/muse_account_api.c"
        "sdk/chat/src/muse_text.c")
    # SDK の Noise XX トランスポート (無改変)
    list(APPEND MUSE_LINK_SRCS
        "sdk/noise_core/src/ClientSession.cpp"
        "sdk/noise_core/src/InitiatorHandshake.cpp"
        "sdk/noise_core/src/PsaCryptoBackend.cpp"
        "sdk/noise_core/src/ServiceCodec.cpp"
        "sdk/noise_core/src/Status.cpp"
        "sdk/noise_core/src/Transport.cpp"
        "sdk/noise_core/src/TransportFrameCodec.cpp")
endif()

idf_component_register(
    SRCS ${MUSE_LINK_SRCS}
    INCLUDE_DIRS "include" "sdk/include" "sdk/minimp3_shim"
    REQUIRES json mbedtls esp-tls esp_http_client nvs_flash lwip wifi
    PRIV_REQUIRES esp_timer
)

if(MUSE_LINK_SRCS)
    target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
    target_compile_options(${COMPONENT_LIB} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    )
endif()
