# Muse chat/Noise 層を ESP-IDF 5.5.1 でコンパイルした結果 (T49 スパイク)

2026-11-18。T47 (`docs/muse-port.md`) の「chat/Noise 層は IDF 5.5.1 のままビルドできる見込み」を
**実コンパイルで確定**させた記録。SDK は読み取り専用で、SAS-Watch 本体ファームには触れていない。

- スパイク: `tools/muse-gadget/spike/` (独立した IDF 5.5.1 プロジェクト、target `esp32s3`)
- SDK: `facebookincubator/muse-gadget-sdk` pinned `ec976f0` (Apache-2.0) / ミラー `.toolchain/muse-gadget-sdk`
- ビルド生成物は `tools/muse-gadget/spike/build/` (gitignore)

---

## 0. 結論 (GO/NO-GO)

| 判定 | 内容 |
|---|---|
| **コンパイル: GO** | `noise_core` 7 ファイル + chat 5 ソース + ヘッダ 10 個が **1 文字も直さずに** ESP-IDF 5.5.1 でコンパイル・リンクできた。**IDF6 専用 API による書き換えは 0 件** |
| **サイズ: GO** | chat/Noise 層の追加は **flash +77,763B**、**静的 DIRAM +3,090B**、**静的 PSRAM +3,864B**、**IRAM 0B**。SAS-Watch の app 空き 2.01MB・内蔵 SRAM 18〜21KB に収まる。**PSA crypto と mbedTLS/esp-tls は SAS-Watch に既にリンク済みなので増分 0** |
| **スタブ: 想定内** | SAS-Watch 側に置き換える関数は **20 個** + 埋め込み MP3 の 2 シンボル + minimp3 シム。どれも T47 で「SAS-Watch 側の実装に差し替える」と書いたもの。**cJSON は IDF 同梱の `json` コンポーネントで足りる (vendor 不要)** |
| **B-min 工数** | T47 の 4〜7 人日 → **4〜6 人日** (コンパイル面の不確実性が消え、B1 の一部が不要に) |
| **残る GO 条件** | 実装ではなく (i) **M1 (資格情報)**: Muse アカウント/VM トークンが外部調達できるか、(ii) **実機 RAM**: TLS ハンドシェイクの内部 RAM と Wi-Fi/BLE 同時 (このスパイクでは未計測) |

NO-GO の根拠は今回見つからなかった。**「IDF 6 が要る」「5.5 ではコンパイルできない」という T47 の懸念は解消**。
B-min の可否はコンパイルではなく、資格情報と実行時メモリの 2 点に絞られた。

---

## 1. 目的とスコープ

| 確かめたこと | 結果 |
|---|---|
| SDK の `noise_core` (Noise XX / PSA crypto / フレーミング) が 5.5.1 でビルドできるか | ✅ ビルド可 (無改変) |
| SDK の chat 層 (`muse_chat_session.cpp` / `muse_chat.c` / `muse_chat_text.c` / `muse_account_api.c`) が 5.5.1 でビルドできるか | ✅ ビルド可 (無改変) |
| 不足する依存は何個で、何に置き換えるのか | ✅ 20 関数 + 2 シンボル + minimp3 シム (§6) |
| flash / 静的 RAM はいくつか | ✅ 実測 (§7) |
| SAS-Watch の予算に載るか | ✅ 載る (§8) |

| 確かめていないこと (B1 以降 / 実機) | 理由 |
|---|---|
| TLS ハンドシェイクの**実行時**内部 RAM ピーク | 実機 (または QEMU) が要る。sdkconfig の `MBEDTLS_INTERNAL_MEM_ALLOC=y` のままリンクは通るが、実行時は T47 §7.3(a) の懸念が残る |
| 実行時の PSRAM 使用量 (バッファ確保) | `muse_hatch_start()` を動かす必要がある。定数から計算した値を §9 に置く |
| Wi-Fi / BLE 同時のヒープ、電池 | 実機。M0/M1 の領域 |
| Muse の API が実際に応答するか | ユーザーの Muse アカウントが要る (M1) |
| 実機での音声→字幕 | 上に同じ |

---

## 2. 環境

| 項目 | 値 |
|---|---|
| ESP-IDF | **v5.5.1** (`.toolchain/esp-idf`, `IDF_TOOLS_PATH=.toolchain/espressif`) |
| ツールチェーン | `xtensa-esp-elf` esp-14.2.0_20241119 |
| target | `esp32s3` (SAS-Watch と同じ) |
| 主な sdkconfig | `CONFIG_SPIRAM=y` / `SPIRAM_MODE_OCT=y` / `SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y` / `FREERTOS_HZ=1000` / `COMPILER_OPTIMIZATION_PERF=y` / `MBEDTLS_INTERNAL_MEM_ALLOC=y` (IDF 既定) / `MUSE_HATCH=y` |
| ネットワーク | 不使用 (cJSON は SDK の managed_components から、minimp3 は SDK からコピー) |
| IDF6 (`.toolchain/esp-idf-v6.0.3`) | 未使用・未変更 |
| 既存 `firmware/build` | 未変更 |

> 注: `MBEDTLS_INTERNAL_MEM_ALLOC=y` は SAS-Watch と同じ。**リンクは通る**が、これは
> 「コンパイルできる」ことの確認であって「実行時に内部 RAM が足りる」ことの確認ではない (§11)。

---

## 3. 段階と到達点

| 段階 | 内容 | 結果 |
|---|---|---|
| S0 | `components/noise_core/` をそのままコピーし、`ClientSession`/`PsaCryptoBackend` を参照してリンク | ✅ ビルド成功。app 318,048B (0x4da60) |
| S1 | + chat 5 ソース (minimp3 は SDK の実物) + cJSON + スタブ | ✅ ビルド成功。app 691,520B (0xa8d40) |
| S2 (B-min 形) | minimp3 をシムに置換 (字幕のみ・音声再生なし) + cJSON は IDF 同梱の `json` | ✅ ビルド成功。app **671,392B** (0xa3ea0)、flash 内訳は §7 |

### 実行コマンド (再現用)

```sh
cd SAS-Watch/tools/muse-gadget/spike
./build.sh            # IDF 5.5.1 を export して idf.py -B build build
./build.sh size       # idf.py size
```

ログ: `tools/muse-gadget/spike/build/last_build_nomp3.log` (gitignore)。
**クリーンビルドでも警告 0 件** (`warning:` の grep が 0)。

---

## 4. 無改変でコンパイルできたもの

sha256 が SDK ミラーと一致することを確認済み (`diff -rq` で `noise_core` も一致)。

| SDK のファイル | 行数 | コピー先 | sha256 (先頭 12 桁) |
|---|---:|---|---|
| `components/noise_core/` 一式 (7 ソース) | 6,739 | `components/noise_core/` | ツリー全体 `cf99bff2ee47` (中身の連結 sha256。ミラーと一致) |
| `components/muse/muse_chat_session.cpp` | 3,019 | `components/muse_chat/src/` | `6a65c3cb0937` |
| `components/muse/muse_chat.c` | 101 | 同 | `542780738f73` |
| `components/muse/muse_chat_text.c` | 376 | 同 | `5863e14ae2a3` |
| `components/muse/muse_account_api.c` | 175 | 同 | `0b9174e2d9fa` |
| `components/muse/muse_text.c` | 320 | 同 | `3866362e50c0` |
| `components/muse` のヘッダ 10 個 | — | `components/muse_api/include/` | すべて一致 |
| `components/minimp3/` (S1 のみ、S2 ではシム) | — | `components/minimp3/` | CC0 (S2 はシム) |

ヘッダ 10 個: `muse_chat.h` / `muse_chat_priv.h` / `muse_account_api.h` / `muse_mem.h` /
`muse_settings.h` / `muse_link.h` / `muse_wifi.h` / `muse_options.h` / `muse_state.h` / `muse_text.h`。

**cJSON は vendor しない**: ESP-IDF 5.5.1 は `json` コンポーネントとして cJSON を同梱しているので、
`REQUIRES json` だけで `cJSON.h` が使える (SDK の `espressif/cjson ~1.7` の vendor は不要)。
S2 のビルドはこちらで測っている。

`sdkconfig` に必要な `CONFIG_MUSE_*` は、SDK の `components/muse/Kconfig` から
**chat が参照する 9 個だけ**を写した (`components/muse_chat/Kconfig`):
`MUSE_HATCH=y` のほか `MUSE_HATCH_COMPACT_BUFFERS` / `MUSE_REALTIME_VOICE` /
`MUSE_REALTIME_VOICE_APP_ID` / `MUSE_LATEST_REPLY` / `MUSE_BOARD_SENSECAP_WATCHER` /
`MUSE_APP_VIEW` / `MUSE_WASM_APPS` / `MUSE_WAKEWORD_ENABLE` (既定 n)。

---

## 5. 発生したエラーと分類

「(a) 5.5 で修正可」「(b) スタブで回避」「(c) IDF6 専用で書き換え要」「(d) 設計上の追加作業」の分類。

| # | 症状 (実エラー) | 原因 | 分類 | 対応 |
|---|---|---|---|---|
| 1 | `ClientSession.h:19:10: fatal error: cstddef: No such file or directory` | `main.c` (C) から C++ ヘッダを include | (a) C/C++ の取り違え。IDF バージョン無関係 | `main.cpp` に改名 |
| 2 | `undefined reference to 'app_main'` | C++ の name mangling | (a) 同上 | `extern "C" void app_main(void)` |
| 3 | `muse_link.h: No such file` + `BUG: component_requirements.py: ... already in the requirements list` | スタブと chat が相互に REQUIRE する循環 | (a) ビルド構成。IDF バージョン無関係 | ヘッダだけの `muse_api` コンポーネントに分離 (ヘッダは無改変) |
| 4 | `undefined reference to muse_settings_hatch_token_len` ほか **20 関数** | SDK の `muse_settings.c`/`muse_link.c`/`muse_options.c`/`muse_state.c`/`main/wifi_mgr.c` が無い | (b) スタブで回避 (§6) | `components/muse_stubs/stub.c` |
| 5 | `undefined reference to _binary_test_reply_mp3_start/_end` | `muse_hatch_mp3_selftest()` が参照する埋め込み MP3 (`EMBED_FILES`) が無い | (b) スタブで回避 | 空のシンボル 2 個 |
| 6 | (S2) minimp3 の `mp3dec_t` が静的 PSRAM を 5.4KB 使う | B-min は音声再生しない | (b) スタブで回避 | `minimp3_shim/minimp3.h` (ヘッダ 3 関数) |
| 7 | **IDF6 専用 API によるコンパイルエラー** | — | **(c) 0 件** | — |
| 8 | 実行時 RAM / TLS の内部 RAM | 未計測 | (d) 設計上の追加作業 | §9 / §11。B1 の最初の実測項目 |

> 分類 (c) が **0 件**だったのが T49 の中心的な結果。T47 で挙げた `mbedtls/private/gcm.h` /
> `psa_crypto_driver_esp_ecdsa*.h` は `main/link_pairing.c` (BLE ペアリング) だけのもので、
> **B-min の chat/Noise 経路には 1 つも現れない**。

---

## 6. スタブ一覧 = B-min で SAS-Watch 側に実装するもの

`components/muse_stubs/stub.c` が定義している 20 関数 + 2 シンボル。
「SDK での実装」が B-min での置換先の目安。

| SDK 関数 | SDK での実装 | B-min での置換 (SAS-Watch) |
|---|---|---|
| `muse_settings_hatch_host(out)` | `components/muse/muse_settings.c` | NVS/`KeyValueStore` の設定 1 行 (既定 `hatch.metaaivm.com`) |
| `muse_settings_hatch_vm(out)` | 同 | 設定 (VM ID。空なら fetch_vms の default) |
| `muse_settings_hatch_token(out)` | 同 | 設定 (トークン。**マスク表示・NVS 暗号化**は §11) |
| `muse_settings_hatch_token_len()` | 同 | 同 (トークン長) |
| `muse_link_hatch_linked()` | `components/muse/muse_link.c` | B-min では常に false (Link ペアリングを使わない) |
| `muse_link_hatch_vm(...)` | 同 | 同上 (B-min は `hatch.token` 経路のみ) |
| `muse_wifi_connected()` | `main/wifi_mgr.c` (muse_wifi) | `firmware/components/wifi` の `wifi::connected()` |
| `muse_options_snapshot` / `clear` / `disconnected` / `pending` / `waiting` / `take` / `result` / `receive` / `receive_message` / `chat_body` / `state_body` / `state_response` (12) | `components/muse/muse_options.c` | B-min では **使わない** (Muse の選択肢 UI)。最初は no-op スタブでよい |
| `muse_state_page(text, &cols, &lines)` | `components/muse/muse_state.c` | `apps/agent` のラベル幅から字幕の折り返しを決める |

埋め込みシンボル 2 個: `_binary_test_reply_mp3_start/_end` (ベンチ用。B-min では空でよい)。

minimp3 シム (`components/minimp3_shim/minimp3.h`) が置き換える API: `mp3dec_t` /
`mp3dec_frame_info_t` / `mp3dec_init()` / `mp3dec_decode_frame()` /
`MINIMP3_MAX_SAMPLES_PER_FRAME` の 5 つ (SDK ソースは無改変のまま)。

---

## 7. サイズ / 静的 RAM 実測

### 7.1 アプリ全体 (同一プロジェクト・同一 sdkconfig、`CONFIG_SPIKE_WITH_CHAT` だけ切り替え)

| ビルド | app bin | 差分 |
|---|---:|---:|
| **S0**: noise_core のみ (chat をコンパイルしない) | 317,520B (0x4da50) | — |
| **S1**: + chat + minimp3 (SDK 実物) | 691,520B (0xa8d40) | +374,000B |
| **S2 = B-min**: + chat + minimp3 シム | **671,392B (0xa3ea0)** | +353,872B |

> S2−S0 の 353,872B は chat 層だけの増分では**ない**。chat が esp-tls / esp_http_client /
> lwip TCP / mbedTLS の TLS スタックを引き込むためで、**その大半は SAS-Watch が既に持っている**。
> SAS-Watch への増分は §7.2 の 77,763B。

### 7.2 SAS-Watch に足す分 (実測、S2 と SAS-Watch の map をオブジェクト単位で比較)

| カテゴリ | 内訳 | サイズ |
|---|---|---:|
| **chat 層** (5 オブジェクト) | `muse_chat_session` 26,425 + `muse_chat_text` 2,120 + `muse_text` 1,552 + `muse_account_api` 1,139 + `muse_chat` 663 | **31,899** |
| **noise_core** (7 オブジェクト) | `ServiceCodec` 7,292 + `InitiatorHandshake` 4,437 + `PsaCryptoBackend` 3,052 + `TransportFrameCodec` 3,032 + `ClientSession` 2,040 + `Transport` 1,536 + `Status` 162 | **21,551** |
| **cJSON** | IDF 同梱 `json` コンポーネント (`REQUIRES json`)。vendor 不要 | **4,861** |
| **FreeRTOS stream buffer** | chat のストリームバッファ (`xStreamBufferCreateWithCaps`)。SAS-Watch は未使用 | **1,657** |
| **libc** | cJSON の数値解析/書式 (`svfscanf` 9,603 + `strtod` 6,131 + gdtoa 1,861 + `sscanf` 200) | **17,795** |
| **合計 (B-min)** | — | **77,763** |
| PSA crypto (13 オブジェクト) | **SAS-Watch で既にリンク済み** → 増分 0 (`psa_crypto.c.obj` ほかが sas_watch.map にある) | **0** |
| mbedTLS / esp-tls / esp_http_client / lwip / cert bundle | 同上 (SAS-Watch の OTA/TLS で既にリンク) | **0** |
| **IRAM** | chat/noise の全オブジェクトが IRAM 0 | **0** |
| **静的 DIRAM** | chat 1,702 + stream_buffer 1,367 + cJSON 20 + noise 1 | **3,090** |
| **静的 PSRAM** (`.ext_ram.bss`) | `muse_chat_session.cpp.obj` 0x340 + 0xf18 | **3,864** |

補足:
- **PSA は T47 の見積り (+25〜35KB) が外れて 0 だった。** SAS-Watch は IDF 5.5 の esp-tls/efuse 経由で
  PSA コアを既にリンクしており、noise_core が使う X25519/AES-GCM/HMAC もその範囲で足りた。
- **cJSON の vendor も不要。** IDF 5.5.1 の `json` コンポーネントで `cJSON.h` が使える
  (S1 は vendor 版、S2 は IDF 版で測り、差は 16B)。
- libc の 17.8KB は cJSON が `sscanf`/`strtod` を使うため。cJSON をやめる選択肢は無い
  (`muse_chat_session.cpp` が cJSON を直接使う) ので、これは付随コストとして計上する。
- minimp3 (TTS/音声再生用) は B-min では不要。実物を入れると **+19,510B / 静的 PSRAM +5.4KB** (S1 と S2 の app 差は +20,112B。S1 は vendored cJSON 版なので cJSON の差 −16B を含む)。

### 7.3 SAS-Watch の予算との突き合わせ

| 資源 | SAS-Watch 現状 | B-min 追加 (実測) | 結果 |
|---|---:|---:|---|
| app (flash) | 空き 2,010,320B (47.9%) | **+77,763B** + 糊/UI 5〜10KB ≈ 83〜88KB | 空き ~1.92MB (46%)。**余裕** |
| 内蔵 SRAM (静的) | 空き 20,489B | **+3,090B** | 空き ~17.4KB。静的 bss の話で、BLE init の 32KB 連続確保とは別 |
| 内蔵 SRAM (実行時) | 18〜21KB | TLS ハンドシェイク次第 (**未計測**) | §8/§10 の最優先実測項目 |
| PSRAM (静的) | — | **+3,864B** | 無視できる |
| PSRAM (実行時) | 肌テーマ空き ~750KB / 最大連続 ~500KB | 素 1.33MB → 要トリム (§8) | **B1 で 250〜300KB まで削る** |
| IRAM | 16,384/16,384 (100%) | **0B** | 影響なし |

> flash の見積りは T47 の 90〜110KB → **実測 78KB (+糊)** に下方修正。

## 8. 実行時 RAM (未測定) と B1 で削るもの

`muse_hatch_start()` の確保は定数から計算できる (実測ではない)。スパイクの設定
(`MUSE_HATCH_COMPACT_BUFFERS=n`) のままでは **PSRAM ≈1.33MB**:

| バッファ | 素 | 内訳 (コード上の定数) |
|---|---:|---|
| 受信スクラッチ 5 本 | 320KB | `SCRATCH=64KB` × 5 |
| MP3 用 | 512KB | `MP3_BUF` (S2 のシムでも**確保だけはされる**) |
| マイク後退 | 256KB | 16kHz×2B×8s |
| デコード出力 | 64KB | 2s |
| NDJSON 行 | 96KB | 16KB × 6 ストリーム |
| タスクスタック | 48KB | PSRAM 上 (`MALLOC_CAP_SPIRAM`) |
| その他 (chunk/note/pcm/texts/queues) | ≈ 40KB | — |
| **合計** | **≈ 1.33MB** | — |

B-min で必要なトリム (B1 の作業):

| 手順 | 変更 | 残り (PSRAM) | 根拠 |
|---|---|---:|---|
| 0 | 素 (スパイクの既定 `MUSE_HATCH_COMPACT_BUFFERS=n`) | **1,336KB** | scratch 320 + MP3 512 + IN 256 + OUT 64 + 行 96 + stack 48 + その他 40 |
| 1 | MP3/デコード/応答バッファを確保しない (音声再生なし) | **760KB** | `MP3_BUF` 512K + `OUT` 64K を `#if` で落とす (−576KB) |
| 2 | マイク後退 8s → 2s (compact 相当) | **568KB** | `IN_BYTES` 256K → 64K (−192KB) |
| 3 | `SCRATCH` 64KB → 16KB | **328KB** | ×5 で −240KB。受信フレーム上限 12〜17KB の範囲 (要実測) |
| 4 | NDJSON 行 16KB → 4KB | **256KB** | ×6 で −72KB |
| 5 | 接続中だけ確保し、切断で解放 | **256KB** | bake (851KB 連続) と共存させるため必須 |

目標: **250〜300KB** (T47 の 200〜280KB とほぼ同じ)。今回の実測 (静的 PSRAM 3.9KB) とも矛盾しない。

---

## 9. GO/NO-GO と B-min の残作業

### 判定

**コンパイル GO。** chat/Noise 層は IDF 5.5.1 で無改変ビルドでき、IDF6 移行は不要。
B-min の残作業は「SAS-Watch 側の糊 (20 関数の置換)」と「実行時 RAM のトリム」で、
どちらも T47 で想定していた範囲。

### 残作業と工数 (更新)

| # | 作業 | 見積り | 根拠 |
|---|---|---:|---|
| 1 | `muse_chat` + `muse_api` + `noise_core` を `firmware/components/` へ (無改変 + `firmware/vendor/README.md` に由来/commit 記載) | 0.25 日 | このスパイクの構成をそのまま使える (§4) |
| 2 | 設定アダプタ (SAS-Watch の NVS/`KeyValueStore` ← `muse_settings_hatch_*` 4 関数) | 0.5 日 | §6 |
| 3 | Wi-Fi 制御 (`wifi::session_start/end` を「画面 ON + 送受信中」に。`muse_wifi_connected` 1 関数) | 0.5〜1 日 | `ota.cpp:118,159` と同じ作法 |
| 4 | トークン保存 + 電話アプリからの投入 (`settings.set` に `muse.token` を追加、マスク) | 0.5 日 | protocol-v1 のキー追加。NVS 暗号化は §11 |
| 5 | 実行時 RAM のトリム (§8) + **実機で TLS 内部 RAM を実測** (ここが GO/NO-GO の実質的な関門) | 1 日 | 未計測。`MBEDTLS_EXTERNAL_MEM_ALLOC` 化の要否をここで決める |
| 6 | 音声アダプタ (16kHz PCM → WAV/base64、20ms チャンク) | 0.5 日 | T47 §5 |
| 7 | UI/字幕 (`apps/agent` へ、15 秒上限、960B ページング、日本語エラー) | 1〜1.5 日 | T47 §8 |
| 8 | 回帰 (core/sim/firmware、`b` ベンチ全テーマ、`int_free`/`psram_free`) | 1 日 | AGENTS のメモリ/描画ルール |
| | **B-min 合計** | **4〜6 人日** | T47 の 4〜7 → 下方修正 (B1 の 2〜4 のうちコンパイル検証が済んだ) |

前提: M1 (資格情報スパイク) が GO であること。M1 が NO-GO なら B は B-full (ペアリング移植) になり、
T47 の推奨どおり **C 継続**。

---

## 10. 未検証・リスク

| # | 項目 | 内容 | いつ潰すか |
|---|---|---|---|
| 1 | TLS の実行時内部 RAM | `MBEDTLS_INTERNAL_MEM_ALLOC=y` のままハンドシェイクが 18〜21KB の空きに収まるか。リンクは通るが実行時は別問題 | B1 の最初 (実機 or QEMU) |
| 2 | PSRAM トリム後の受信耐性 | `SCRATCH` 16KB / 行 4KB で VM の長い応答やイベントが欠けないか | B1 (実機 + M1 のトークン) |
| 3 | Wi-Fi + BLE 同時のヒープ | SAS-Watch は BLE 常駐。Wi-Fi 初期化と合わせた内部ヒープ | B1 |
| 4 | M1 (資格情報) | `mgst_` / ユーザートークンで VM トークンが得られるか | T47 §9.4 M1 |
| 5 | 実機の音声/字幕 | マイク 16kHz → WAV → 応答字幕 | B1/B2 + M0 |
| 6 | 上流 SDK の追随 | pinned `ec976f0` から動いたときの差分 | 移植時に `vendor/README.md` へ記録 |
| 7 | ライセンス | Apache-2.0 の表示義務 (コピーしたファイルのヘッダは保持済み) | 実装時 |

---

## 11. 付録: 構成ファイルと再現

```
tools/muse-gadget/spike/
  build.sh                       # IDF 5.5.1 を export してビルド
  CMakeLists.txt                 # project(muse_chat_spike)
  sdkconfig.defaults             # esp32s3 / 32MB / octal PSRAM / PERF
  sdkconfig.defaults.s0          # S0 基準ビルド用 (CONFIG_SPIKE_WITH_CHAT=n)
  main/                          # 参照コード (実機用ではない)
  components/
    noise_core/                  # SDK の完全コピー (無改変)
    muse_api/include/            # SDK ヘッダ 10 個 (無改変)
    muse_chat/src/               # SDK ソース 5 個 (無改変)
    muse_chat/Kconfig            # chat が参照する CONFIG_MUSE_* 9 個
    muse_stubs/stub.c            # 20 関数 + 2 シンボル
    minimp3_shim/                # B-min 用 (音声再生なし)
  measurements/                  # 実測の生データ (CSV/ログ。再現の証拠として残す)
  build/ build-s0/               # gitignore (ビルド生成物)
```

ビルド:
```sh
cd SAS-Watch/tools/muse-gadget/spike && ./build.sh
```

サイズ (chat あり = B-min):
```sh
cd SAS-Watch/tools/muse-gadget/spike && ./build.sh size
# もしくは
.toolchain/espressif/python_env/idf5.5_py3.14_env/bin/python \
  .toolchain/esp-idf/tools/idf_size.py --archives \
  SAS-Watch/tools/muse-gadget/spike/build/muse_chat_spike.map
```

S0 (chat なし) の基準ビルドと差分:
```sh
cd SAS-Watch/tools/muse-gadget/spike
idf.py -B build-s0 -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-s0/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.s0" build
# 2 つの map を idf_size.py --files --format csv で出してオブジェクト単位で比較
```

実 MP3 デコード (将来 TTS を足す場合): `components/minimp3_shim/` を消し、SDK の
`esp32/components/minimp3/` を `components/minimp3/` にコピーして `muse_chat` の
`REQUIRES` を `minimp3_shim` → `minimp3` に変更 (+19,510B / 静的 PSRAM +5.4KB)。
