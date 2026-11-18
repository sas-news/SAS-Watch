# T49 spike — Muse chat/Noise 層を ESP-IDF 5.5.1 でコンパイルする

`docs/muse-chat-spike.md` の検証用プロジェクト。**SAS-Watch 本体には組み込まない。**
SDK ミラー (`.toolchain/muse-gadget-sdk`, pinned `ec976f0`) から、chat/Noise に必要な
ファイルだけを **無改変で** コピーし、SAS-Watch 側の設定/リンク/Wi-Fi/字幕ページに
あたる部分だけをスタブして、ESP-IDF **5.5.1** でビルドできるかを確かめる。

## ビルド

```sh
./build.sh          # build/ に生成 (gitignore)
./build.sh size     # サイズ表示
./build.sh fullclean
```

workspace の `.toolchain/esp-idf` (v5.5.1) と `.toolchain/espressif`
(IDF_TOOLS_PATH) を使う。IDF6 (`.toolchain/esp-idf-v6.0.3`) は使わない。
ネットワークアクセスは不要。

chat を外した基準ビルド (S0) は `docs/muse-chat-spike.md` §11 のコマンド
(`build-s0/` に作る)。

## 構成

| パス | 中身 |
|---|---|
| `components/noise_core/` | SDK `esp32/components/noise_core` の完全コピー (無改変) |
| `components/muse_api/include/` | SDK `components/muse` のヘッダ 10 個 (無改変) |
| `components/muse_chat/src/` | SDK の `muse_chat.c` / `muse_chat_session.cpp` / `muse_chat_text.c` / `muse_account_api.c` / `muse_text.c` (無改変) |
| `components/muse_stubs/` | SAS-Watch 側に置き換わる 20 関数 + 埋め込み MP3 のシンボル 2 個のスタブ |
| `components/minimp3_shim/` | B-min (字幕のみ・音声再生なし) 用の minimp3 代替ヘッダ |
| `main/` | リンクさせるためだけの参照コード |

cJSON は **ESP-IDF 同梱の `json` コンポーネント**を使う (vendor しない)。
SDK ソース/ヘッダがミラーと同一であることは sha256 で確認済み
(`docs/muse-chat-spike.md` §4)。

## 結果

- ビルド結果・エラー分類・サイズ/RAM・GO/NO-GO は `docs/muse-chat-spike.md`。
- 要点: SDK の chat 5 ソース + noise_core 7 ソースが **無改変でコンパイル・リンク**。
  IDF6 専用 API による書き換えは 0 件。SAS-Watch への増分は flash +77,763B /
  静的 DIRAM +3,090B / 静的 PSRAM +3,864B / IRAM 0。
