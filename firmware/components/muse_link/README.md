# muse_link — Muse chat/Noise 層の土台 (T50 Phase 1)

Meta の [Muse Gadget SDK](https://github.com/facebookincubator/muse-gadget-sdk)
(pinned **`ec976f0`**, Apache-2.0) の「Muse と話す」層を SAS-Watch に組み込むための
コンポーネント。**Phase 1 は土台のみ**で、既定は無効 (`CONFIG_WATCH_ENABLE_MUSE_LINK=n`)。

- 設計・サイズ実測・Phase 2 の残作業: [`docs/muse-bmin.md`](../../../docs/muse-bmin.md)
- 移植可否の調査: [`docs/muse-port.md`](../../../docs/muse-port.md)
- IDF 5.5.1 でのコンパイル検証: [`docs/muse-chat-spike.md`](../../../docs/muse-chat-spike.md)
- スパイク (独立プロジェクト): [`tools/muse-gadget/spike/`](../../../tools/muse-gadget/spike/)

## 既定ビルドへの影響

`CONFIG_WATCH_ENABLE_MUSE_LINK=n` (既定) では `CMakeLists.txt` の `SRCS` が空になり、
このコンポーネントは**ヘッダだけの存在**になる。バイナリへの追加は **0B**
(`sas_watch.bin` は 2,184,064B のまま)。起動時の処理も `app_main.cpp` の
`#if CONFIG_WATCH_ENABLE_MUSE_LINK` で丸ごと外れる。

## 有効にしたとき

```sh
cd firmware
# sdkconfig (または menuconfig) で
#   Component config > muse_link (Muse chat/Noise, Phase 1) > Muse chat/Noise layer
# を有効にする
idf.py build
```

Phase 1 で起動時にやること:

1. NVS namespace `muse` の `host` / `vm_id` / `token` を読む (README のスキーマ)。
2. `muse_link` タグで host / vm / token の有無をログに出す (**token の値は出さない**)。
3. それだけ。Wi-Fi / TLS / Noise セッション / セッションタスクは作らない。

実測 (T50, IDF 5.5.1): `sas_watch.bin` 2,184,064B → **2,281,312B (+97,248B)**、
静的 DIRAM 321,271 → 327,047B (空き 20,489 → 14,713B)、静的 PSRAM +4,704B、IRAM 0B。
内訳は `docs/muse-bmin.md` §3。

資格情報の投入 (NVS 書き込み) は Phase 2。Phase 1 では読み出しのみ。

## NVS スキーマ (namespace `muse`)

SDK の `components/muse/muse_settings.c` と同じキー名に揃えている
(公式ファームの NVS ダンプからそのまま移せる: docs/muse-bmin.md §M1)。

| key | 型 | 内容 |
|---|---|---|
| `host` | str | VM のホスト。未設定なら SDK 既定 `hatch.metaaivm.com` |
| `vm` | str | VM ID (空 = アカウント既定 VM)。旧 Phase 1 の `vm_id` も読み出しのみ受ける |
| `token` | str | device token または VM token (最大 1023B)。**ログ・UI・シリアル出力に出さない** |

値はコードにもドキュメントにも埋めない。投入・確認・消去は T52 で追加した
シリアル `muse` コマンド (§シリアル投入) か `muse_link_provision()`。

## シリアル投入 (T52)

USB-Serial/JTAG のモニタで **大文字 `M`** を送ると行モードに入る
(小文字 `m` は従来どおり More へ遷移)。改行までを 1 行として解釈する。

```
M
muse status
muse set host hatch.metaaivm.com
muse set vm <vm-id>
muse set token <token>
muse clear
```

- 行モードは 30 秒入力が無ければ自動で抜ける。最大 1100B/行 (token 1023B 用)。
- 行の内容はログに出さない (token を含みうる)。長さだけ出す。
- `muse set <field>` の値を空にするとその項目だけ消す。
- 反映は即時 (NVS 保存 + メモリ内キャッシュ更新)。再起動は不要。
- 有効化は `CONFIG_WATCH_ENABLE_MUSE_LINK=y` が必要 (既定 off)。off のとき
  `M` は行モードにならず、従来どおり More へ遷移する。

`muse status` の出力例 (token は長さのみ):

```
{"configured":true,"ready":false,"wifi":false,"host":"hatch.metaaivm.com","vm":"...","token_len":42}
```

## 中身

| パス | 出どころ | 備考 |
|---|---|---|
| `sdk/noise_core/src/*.cpp` (7) | SDK `esp32/components/noise_core/src` | **無改変** (Apache-2.0) |
| `sdk/include/xplat/noise/core/*.h` | 同 `include/xplat/...` | **無改変** |
| `sdk/chat/src/*.c|cpp` (5) | SDK `esp32/components/muse` | **無改変**。`muse_chat.c` / `muse_chat_session.cpp` / `muse_chat_text.c` / `muse_account_api.c` / `muse_text.c` |
| `sdk/include/*.h` (10) | 同 `components/muse` のヘッダ | **無改変**。MbedtlsCryptoBackend (IDF6 専用ヘッダ使用) はコピーしない |
| `sdk/minimp3_shim/minimp3.h` | T49 スパイク | 音声再生をしない B-min 用シム (実 MP3 デコードは Phase 3) |
| `src/muse_link.cpp` | SAS-Watch | 公開 API + NVS 読み出し |
| `src/muse_adapters.cpp` | SAS-Watch | SDK が要求する 20 関数 + MP3 シンボル 2 個 + リンク維持表 |
| `include/muse_link/muse_link.h` | SAS-Watch | 公開 API (`muse_link_init` / `ready` / `status` / `chat`) |

sha256 (SDK ミラーと一致することを確認済み。詳細は `docs/muse-bmin.md`):

```
6a65c3cb0937f8d50701fb125a701ed4edaf0f63e885eaf79b928bde14e28772  sdk/chat/src/muse_chat_session.cpp
542780738f73beb2d03ca7a19504ea8b6bc1a4b343c868c3e8b84fb2329ce930  sdk/chat/src/muse_chat.c
5863e14ae2a356f5e415fa1e61a3e6b5c5a07b049909a96954a5f659acf1247c  sdk/chat/src/muse_chat_text.c
0b9174e2d9fa6d9d68431ef83a4cadf106c3de0b10ae1d3b2db9ca7b6469d523  sdk/chat/src/muse_account_api.c
3866362e50c085e27f7f2dc6be1888e6ca37aa171408eac3e23e7dbf4491381e  sdk/chat/src/muse_text.c
```

noise_core はディレクトリ内の全ファイルの中身を連結した sha256 が
`cf99bff2ee479ae052549eeebaab1936d28af753e3411d3f28e08629ee25b7ff` (ミラーと一致)。

## Wi-Fi on-demand (T54 Phase 2 ②)

Muse は使うときだけ Wi-Fi を上げ、使い終わったら完全に落とす (常時接続しない)。

```c
if (muse_link_wifi_ensure(20000)) {      // app タスクから呼ぶ
    /* ... 送受信のたびに muse_link_wifi_touch() ... */
    muse_link_wifi_poll();               // app タスクのループで回す
}
muse_link_wifi_release();                // 自分が上げたときだけ落ちる
```

- 所有権は `muse_link` 側で管理する。OTA 等が上げている Wi-Fi は落とさない
  (逆に OTA が落とした場合は次の `ensure` で上げ直す)。
- アイドル `CONFIG_WATCH_MUSE_WIFI_IDLE_S` (既定 60 秒、10〜600) で release。
  自動タイマータスクは作らない (`poll()` を呼ぶ側が回す)。
- `wifi::session_start` は最大 timeout ブロックするので **LVGL タスクから呼ばない**。

## 音声アダプタ (T54 Phase 2 ④)

`include/muse_link/muse_link_voice.h`。16kHz/16bit/mono PCM → WAV/base64 の逐次変換
(SDK の音声ノート形式: RIFF 44B、RIFF/data サイズは 0xFFFFFFFF のストリーミング、
JSON は `data_base64` / `audio/wav` / `voice_note.wav`)。

```c
muse_link_voice_wav_header(h, MUSE_LINK_VOICE_SAMPLE_RATE);   // prefix の後
muse_link_voice_b64_init(&b);
/* PCM チャンクごとに */
muse_link_voice_b64_feed(&b, pcm, bytes, out, cap, &written);
muse_link_voice_b64_finish(&b, out, cap, &written);           // 最後に '=' を出す
```

- **PCM 全部・base64 全部を一度に持たない** (逐次変換、確保なし)。
- 上限 60 秒 (PCM 1,920,000B)。Muse は 15 秒で打ち切るので UI は 15 秒を推奨。
- 実送信は Phase 2 ③ (セッション実装後)。

## テスト (ホスト)

```sh
firmware/components/muse_link/test/run.sh
```

- `test_muse_console.c`: シリアル `muse ...` 行のパーサ (T52)
- `test_muse_voice.c`: WAV ヘッダ / base64 (RFC 4648 ベクタ + 逐次一致) / 上限 /
  JSON エンベロープ (T54)

ESP-IDF も実機も要らない (`cc -std=c99 -Wall -Wextra -Werror`)。

## アダプタの TODO

- `muse_link_hatch_linked()` / `muse_link_hatch_vm()`: 常に false (B-min は
  Muse アプリの BLE ペアリングを使わない)。TODO(muse): M1 で token が得られない
  場合は B-full (ペアリング移植) を再検討。
- `muse_wifi_connected()`: `wifi::connected()`。TODO(hw): Phase 2 で
  「画面 ON + 送受信中だけ Wi-Fi」の状態を返す。
- `muse_state_page()`: 固定 20 桁 × 8 行。TODO(hw): AI 画面の実効幅で調整。
- `muse_options_*`: 全 no-op (選択肢 UI はスコープ外)。TODO(muse): Phase 3。
- リンク維持表 `s_link_keep`: Phase 1 はセッションを開始しないため、
  chat オブジェクトがリンカに落とされないよう参照するだけ。TODO(muse):
  Phase 2 でセッション開始を実装したら削除。

## ライセンス

`firmware/components/muse_link/sdk/` 配下は Meta Platforms, Inc. and affiliates の
Apache License 2.0 (各ファイルのヘッダを保持)。SAS-Watch 側のファイル
(`src/`, `include/`, `CMakeLists.txt`, `Kconfig`) は SAS-Watch のライセンスに従う。
