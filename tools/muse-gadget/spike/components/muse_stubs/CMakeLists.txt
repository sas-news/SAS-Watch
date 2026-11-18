# T49 spike: SDK components/muse の代わりに置くスタブ。
# SDK の設定/リンク/選択肢 UI/字幕ページを、SAS-Watch 側の実装に差し替える前提。
idf_component_register(
    SRCS "stub.c"
    REQUIRES muse_api json
)
