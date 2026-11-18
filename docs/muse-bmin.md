# Muse chat/Noise 層の SAS-Watch 統合 — Phase 1 (土台のみ, 既定 off) (T50)

2026-11-18。T49 (`docs/muse-chat-spike.md`) の GO を受けて、Meta の Muse Gadget SDK
(pinned `ec976f0`, Apache-2.0) の **chat/Noise 層を SAS-Watch 本体ファームへ Phase 1 だけ**
統合した記録。実接続はまだしない (M1 の資格情報が無いため)。

- Phase 1 の範囲: コンポーネント追加 / Kconfig (既定 off) / NVS スキーマの読み出し /
  起動配線。**ネットワーク接続・セッションタスク・UI 統合・音声アダプタは Phase 2。**
- 既定ビルドは **追加 0B・サイズ不変** (実測: 2,184,064B のまま)。
- 関連: `docs/muse-port.md` (方式比較 A/B/C) / `docs/muse-chat-spike.md` (IDF5.5.1 での
  コンパイル検証) / `tools/muse-gadget/spike/` / `firmware/components/muse_link/README.md`

---

## 0. 結論

| 項目 | 結果 |
|---|---|
| 既定ビルド (Kconfig off) | **sas_watch.bin = 0x215380 (2,184,064B) → 変更前と同一 (+0B)**。静的 DIRAM 321,271B / IRAM 16,384B も不変 |
| Kconfig on ビルド | **0x22cf60 (2,281,312B) = +97,248B**。Flash Code +86,208 / .rodata +8,840 / 静的 PSRAM +4,704 / 静的 DIRAM +5,776 |
| 起動時の挙動 (on) | NVS `muse` を読んでログに出すだけ。ネットワーク・タスク・大確保は**しない** |
| 資格情報 | NVS namespace `muse` の `host` / `vm_id` / `token` を**読むだけ**。値はコードに埋めない |
| 既存機能への影響 | core 273/273 pass、sim 監査 0 件、既定ビルドサイズ不変。BLE/LVGL/電池のコードは未変更 |
| 判定 | **Phase 1 完了 (GO)**。Phase 2 の残作業は §6 (4〜6 人日) |
| T52 追記 | **Phase 2 ① (資格情報の投入・確認・消去) 実装済み**。on ビルドは 0x22dd50 (+100,816B vs off)。詳細は §9。既定 off は 0x215380 のまま (不変) |

> **T49 の数字の訂正**: T49 は「SAS-Watch への増分 = flash +77,763B / PSA 増分 0」と見積もったが、
> 本統合の実測は **+97,248B**。PSA crypto は SAS-Watch にリンク済みでも、noise が使う
> **X25519 ECDH / AES-GCM / HMAC の PSA ドライバ・アルゴリズム実装**が新たにリンクされ
> `libmbedcrypto.a` が +19,455B 増える (§3.2)。T49 の「PSA 0」は誤りだった。
> 差の内訳は §3 に実測で示す。

---

## 1. Phase 1 で追加したもの

```
firmware/components/muse_link/
  CMakeLists.txt              # Kconfig off で SRCS 空 (0B)、on で SDK コピー+アダプタをコンパイル
  Kconfig                     # CONFIG_WATCH_ENABLE_MUSE_LINK (default n)
  README.md                   # 由来・sha256・有効化・TODO 一覧
  include/muse_link/muse_link.h   # 公開 API (init / ready / status / chat)
  src/muse_link.cpp           # NVS 読み出し + 公開 API
  src/muse_link_internal.h
  src/muse_adapters.cpp       # SDK が要求する 20 関数 + MP3 シンボル 2 個 + リンク維持表
  sdk/chat/src/*.c|cpp (5)    # SDK コピー (無改変)
  sdk/include/*.h (10)        # SDK コピー (無改変)
  sdk/include/xplat/noise/core/*.h (13)
  sdk/noise_core/src/*.cpp (7)# SDK コピー (無改変)
  sdk/minimp3_shim/minimp3.h  # T49 の B-min 用シム (音声再生なし)
```

変更した既存ファイル (最小):

| ファイル | 変更 |
|---|---|
| `firmware/main/CMakeLists.txt` | `PRIV_REQUIRES` に `muse_link` を 1 行追加 |
| `firmware/main/app_main.cpp` | `#if CONFIG_WATCH_ENABLE_MUSE_LINK` でヘッダ include と `muse_link_init()` 呼び出しを追加 (既存コードは無変更) |

SDK ソース/ヘッダはミラーと sha256 一致 (無改変)。`MbedtlsCryptoBackend` (IDF6 専用ヘッダを
使う) はコピーしない。詳細は `firmware/components/muse_link/README.md`。

## 2. Kconfig / 有効化

`Component config > muse_link (Muse chat/Noise, Phase 1) > Muse chat/Noise layer`
(**default n**)。

```sh
cd firmware
# 既定 (off): 何も変わらない
idf.py build

# 有効化 (検証時は専用 build ディレクトリで)
idf.py menuconfig   # 上記を y
idf.py build
```

`CONFIG_WATCH_ENABLE_MUSE_LINK=n` (既定) のとき:

- `muse_link` コンポーネントの `SRCS` が空 → **コンパイル対象 0**、リンクされるコード 0B。
- `app_main.cpp` の include と `muse_link_init()` はプリプロセッサで外れる。
- Kconfig の n bool は sdkconfig に書き出されないため、既存 `sdkconfig` の値も変わらない。

## 3. サイズ実測

計測環境: ESP-IDF **v5.5.1** (`xtensa-esp-elf 14.2.0_20241119`) / target esp32s3 /
`IDF_TOOLS_PATH=.toolchain/espressif` / `IDF_COMPONENT_CACHE_PATH=.toolchain/comp-cache`。
同一ツリー・同一 sdkconfig で、`CONFIG_WATCH_ENABLE_MUSE_LINK` の y/n だけを変えてビルド。

```sh
# off
idf.py -B build-verify -DSDKCONFIG=build-verify/sdkconfig build
# on
cp sdkconfig build-verify-muse/sdkconfig && echo 'CONFIG_WATCH_ENABLE_MUSE_LINK=y' >> build-verify-muse/sdkconfig
idf.py -B build-verify-muse -DSDKCONFIG=build-verify-muse/sdkconfig build
```

### 3.1 全体

> この表は **T50 Phase 1 時点**の実測。T52 (Phase 2 ①) を足した最新値は §9.4。

| 指標 | off (既定) | on | 差 |
|---|---:|---:|---:|
| `sas_watch.bin` | **2,184,064B (0x215380)** | **2,281,312B (0x22cf60)** | **+97,248B** |
| app 空き (4MB slot) | 2,010,240B (47.9%) | 1,912,992B (45.6%) | −97,248B |
| Flash Code (.text) | 1,459,840 | 1,546,048 | +86,208 |
| Flash Data (.rodata) | 539,236 | 548,076 | +8,840 |
| 静的 PSRAM (.ext_ram.bss) | 55,352 | 60,056 | +4,704 |
| **静的 DIRAM (内蔵)** | **321,271 / 341,760 (空き 20,489)** | **327,047 / 341,760 (空き 14,713)** | **+5,776** |
| IRAM | 16,384 / 16,384 (100%) | 16,384 / 16,384 (100%) | **0** |

DIRAM の内訳差: .bss +3,576 / .text +2,168 / .data +32。
.bss の +3,576 は `muse_link.cpp` の NVS キャッシュ (host/vm/token ≈1.2KB) と FreeRTOS
stream buffer の静的領域など。.text +2,168 は内蔵 RAM 常駐コード (FreeRTOS の一部)。

### 3.2 何が増えたか (per-archive 実測)

| アーカイブ | 増分 | 中身 |
|---|---:|---|
| `libmuse_link.a` | **+55,098** | 下記内訳 |
| `libmbedcrypto.a` | **+19,455** | noise が使う **PSA の X25519 ECDH / AES-GCM / HMAC-SHA256** の実装・ドライバ (SAS-Watch は PSA コアを持っていたが、このアルゴリズム群は未リンクだった) |
| `libc.a` | **+15,866** | cJSON の数値解析 (`svfscanf` / `strtod` / gdtoa) |
| `libesp_app_format.a` | +5,845 | **merged `.rodata.str1.4` の帰属アーティファクト**。実体は今回のコードの文字列 (ログ・JSON テンプレート)。ホスト側で計上した rodata +8,840 の大半 |
| `libjson.a` | +4,917 | cJSON (IDF 同梱 `json` コンポーネント。vendor 不要) |
| `libfreertos.a` | +2,259 | `xStreamBufferCreateWithCaps` 等 |
| `liblwip.a` / `esp_http_client` / `esp-tls` / ほか | 約 +1,600 | ソケット・HTTP の追加分 (大半は既存 OTA でリンク済み) |
| 合計 (アーカイブ Δの和) | ≈ +104,956 | bin 差 +97,248 との差はアーティファクト/アラインメント |

`libmuse_link.a` (+55,098) の内訳 (per-file):

| 区分 | サイズ |
|---|---:|
| chat 5 ソース (`muse_chat_session` 26,164 / `muse_chat_text` 2,140 / `muse_text` 1,552 / `muse_account_api` 1,179 / `muse_chat` 336) | **31,371** |
| noise_core 7 ソース (`ServiceCodec` 7,412 / `InitiatorHandshake` 4,497 / `PsaCryptoBackend` 3,076 / `TransportFrameCodec` 3,068 / `ClientSession` 2,088 / `Transport` 1,540 / `Status` 162) | **21,843** |
| `muse_adapters.cpp` (20 関数 + リンク維持表) | 248 |
| `muse_link.cpp` (NVS + 公開 API) | 1,636 |

> **T49 との差**: T49 の 77,763B は、PSA のアルゴリズム実装 (+19,455B) と merged-rodata
> の文字列増 (+5,845B) を数えていなかった。実統合の増分は **+97,248B** が正しい。

### 3.3 実行時メモリ (Phase 1 では増えない)

- Phase 1 は **セッションタスクを作らない** ため、T47 で見積もった PSRAM の大確保
  (素 1.33MB) は発生しない。増えるのは静的 PSRAM +4,704B と上記 static のみ。
- ただし **Kconfig on の static DIRAM 空きは 14,713B** (off は 20,489B)。実行時の内蔵ヒープ
  空き 18〜21KB も同程度に減るため、**Phase 2 で TLS を張る前に mbedTLS のメモリ配置
  (PSRAM 化 or 動的バッファ) の実測が必要** (T47 §7.3(a))。

## 4. NVS スキーマ (SDK と同じキー名)

namespace `muse`。SDK の `components/muse/muse_settings.c` と同じキー名に揃えた
(`muse_settings.c:36` で `#define NS "muse"`、`:155-157` で `host` / `vm` / `token` を
load、`:315-340` で save)。**公式ファームの NVS ダンプからそのまま移せる** (§10)。

| key | 型 | 内容 |
|---|---|---|
| `host` | str | VM のホスト。未設定なら SDK 既定 `hatch.metaaivm.com` |
| `vm` | str | VM ID (空 = アカウント既定 VM)。Phase 1 の旧キー `vm_id` は**読み出しのみ**後方互換 |
| `token` | str | device token または VM token。最大 **1023B** (SDK の `MUSE_TOKEN_MAX`)。**ログ・UI・シリアル出力に出さない** |

値はコード・ドキュメントに埋めていない。読み書きは T52 の
`muse_link_provision()` / `muse_link_clear()` とシリアル `muse` コマンド (§9)。

## 5. 公開 API と Phase 1 の挙動

`include/muse_link/muse_link.h`:

| API | Phase 1 の挙動 |
|---|---|
| `esp_err_t muse_link_init(void)` | NVS を読んでキャッシュし、`muse_link` タグで host/vm/token 有無をログ。**ネットワーク/タスク/大確保なし**。未設定でも `ESP_OK` |
| `bool muse_link_ready(void)` | 「token と host があり、Wi-Fi 接続中」。Phase 1 では Wi-Fi を張らないので実質 false |
| `void muse_link_status(muse_link_status_t*)` | 秘密を含まない状態 (host/vm_id/configred/ready/wifi) |
| `esp_err_t muse_link_chat(const char*, cb, ctx)` | **ESP_ERR_NOT_SUPPORTED** (TODO(muse): Phase 2 の後半で `/chat/stream` + `/chat/subscribe`) |
| `bool muse_link_provision(host, vm, token)` | **T52**: NVS に保存 (NULL=変更なし / ""=その項目を消す)。長さ・文字種を検証し、1 つでも不正なら何も書かない |
| `void muse_link_clear(void)` | **T52**: NVS namespace `muse` を全消去 (再起動不要) |
| `bool muse_link_get_status(char*, size_t)` | **T52**: `{"configured":..,"ready":..,"wifi":..,"host":"..","vm":"..","token_len":N}`。token は長さのみ |
| `bool muse_link_console_line(const char*)` | **T52**: シリアル 1 行 (`muse ...`) を解釈して実行。watch_app の serial_dbg_task 専用 |

アダプタ 20 関数の Phase 1 の値: settings は NVS キャッシュ、link は false、wifi は
`wifi::connected()`、options は全 no-op、`muse_state_page` は 20 桁 × 8 行。
未確定は `TODO(hw)` / `TODO(muse)` を付けている (`firmware/components/muse_link/README.md`)。

`muse_adapters.cpp` の `s_link_keep[]` は、Phase 1 がセッションを開始しないため
chat オブジェクトがリンカに落とされないよう参照するだけの表 (実行しない)。
TODO(muse): Phase 2 でセッション開始を実装したら削除する。

## 6. Phase 2 の残作業と工数

| # | 作業 | 見積り | 備考 |
|---|---|---:|---|
| 1 | ~~資格情報の投入経路~~ **完了 (T52)** | ~~0.5 日~~ | シリアル `muse` コマンドで投入・確認・消去。BLE `settings.set` 経路は未実装 (シリアルのみ) |
| 2 | Wi-Fi の on-demand 制御 (`wifi::session_start/end` を「画面 ON + 送受信中」だけ) | 0.5〜1 日 | `ota.cpp:118,159` と同じ作法。電池の要 |
| 3 | セッション開始 (`muse_hatch_start` 相当) + **RAM トリム** + **実機 TLS 内部 RAM 実測** | 1.5〜2 日 | ここが実質の GO/NO-GO。素の 1.33MB PSRAM を 250〜300KB に削る。mbedTLS の PSRAM 化 or 動的バッファを決める |
| 4 | 音声アダプタ (16kHz PCM → WAV/base64、20ms チャンク) | 0.5 日 | T47 §5。スピーカー無効・字幕のみ |
| 5 | UI 統合 (`apps/agent` の partial/reply へ、15 秒上限、960B ページング、日本語エラー) | 1〜1.5 日 | T47 §8。core/protocol の変更は不要 |
| 6 | 回帰 (core/sim/firmware、`b` ベンチ全テーマ、`int_free`/`psram_free`) | 1 日 | AGENTS のメモリ/描画ルール |
| | **合計 (T52 後)** | **3.5〜5.5 人日** | T50 の 4〜6 人日から Phase 2 ① (0.5 日) を引いた残り。②〜⑥ は未着手 |

前提: **M1 (資格情報) が GO**。M1 が NO-GO なら B-min は成立せず、T47 の推奨どおり C 継続
(B-full のペアリング移植は 9〜15 人日)。

## 7. リスク

| # | リスク | 内容 / 対応 |
|---|---|---|
| R1 | **Kconfig on の内蔵 RAM** | static 空き 20,489→14,713B。TLS ハンドシェイクのピーク (30〜50KB 級) は現状のままでは足りない見込み。Phase 2 #3 で `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` 化 or 動的バッファ + content len 縮小を実測して決める |
| R2 | **PSRAM の連続ブロック** | セッション開始時は素で 1.33MB を確保する。SAS-Watch の肌テーマは最大連続 ~500KB なので、接続時確保 + バッファ縮小が必須 (Phase 2 #3) |
| R3 | **Wi-Fi 常時接続の電池** | SAS-Watch は Wi-Fi を OTA 時だけ起動する設計。Muse では「画面 ON + 送受信中」に限定する。実測は未 (TODO(hw)) |
| R4 | **資格情報 (M1)** | device token が Muse アプリの BLE ペアリングでしか得られない可能性。M1 の結果次第で B-min の前提が崩れる |
| R5 | **NVS 平文の token** | `muse/token` は NVS 平文。物理アクセスで読める。NVS 暗号化 (HMAC eFuse) か暗号化保存を Phase 2 で検討。デバッグログには値を出さない実装済み |
| R6 | **リポジトリの `dependencies.lock` が旧絶対パス** | 本ワークスペース移動前の `/home/sasnews/Desktop/ai-space/SAS-Watch/...` が 3 か所残っており、**現パスでは `idf.py build` が configure で失敗する** (§8)。本タスクでは検証時だけ一時的にパスを直し、終了後に元へ戻した (git クリーン)。**Lead 判断で lock を再生成するのが根本対応** |
| R7 | リンク維持表 | Phase 1 だけの仕掛け。Phase 2 でセッションを実装したら削除する (TODO(muse)) |
| R8 | 上流 SDK の追随 | pinned `ec976f0`。移植差分は `firmware/components/muse_link/README.md` に記録済み。更新時は sha256 を再確認 |
| R9 | 実機未確認 | Kconfig on での起動ログ・メモリは**実機未確認** (TODO(hw))。Phase 1 は NVS 読みのみなので risk は低いが、`muse_link_init()` の戻り値は現状 app_main で無視している (失敗しても時計は動く) |

## 8. 検証結果と再現手順

### 8.1 実施結果

| 検証 | 結果 |
|---|---|
| 既定 build (Kconfig off) | ✅ `0x215380` / 2,184,064B — **変更前と同一** |
| Kconfig on build | ✅ `0x22cf60` / 2,281,312B (+97,248B) |
| core | ✅ **273/273 pass** (`ctest --test-dir core/build`) |
| sim | ✅ ビルド成功・`snapshot` 監査 **0 件** |
| `managed_components/` | 未編集 |
| 既存 firmware コンポーネント | 未変更 (main の 2 ファイルのみ) |
| コンパイルされた `muse_link` オブジェクト | off: **0 個** (Components 一覧に出るだけ) / on: **14 個** (chat 5 + noise 7 + アダプタ 2) |
| `muse_link` のコンパイル警告 | **0 件** (既存 110 件の警告 — ui の `-fno-rtti` 等 — は off ビルドと同一) |

ログ (gitignore 済みの測定物): `tools/muse-gadget/spike/measurements/t50/`
(`off_real.log` / `on_real.log` / `arch_off.csv` / `arch_on.csv`)。

### 8.2 ビルドコマンド

```sh
cd SAS-Watch/firmware
export IDF_PATH=$PWD/../../.toolchain/esp-idf
export IDF_TOOLS_PATH=$PWD/../../.toolchain/espressif
export IDF_COMPONENT_CACHE_PATH=$PWD/../../.toolchain/comp-cache
. "$IDF_PATH/export.sh"

# 既定 (off)
idf.py -B build-verify -DSDKCONFIG=build-verify/sdkconfig build

# on
mkdir -p build-verify-muse && cp sdkconfig build-verify-muse/sdkconfig
echo 'CONFIG_WATCH_ENABLE_MUSE_LINK=y' >> build-verify-muse/sdkconfig
idf.py -B build-verify-muse -DSDKCONFIG=build-verify-muse/sdkconfig build
```

### 8.3 `dependencies.lock` の件 (要 Lead 判断)

本ワークスペースでは、追跡ファイル `firmware/dependencies.lock` に移動前の絶対パスが
残っているため、素の `idf.py build` が次のエラーで止まる:

```
ERROR: The "path" field in the manifest file
"/home/sasnews/Desktop/ai-space/SAS-Watch/firmware/vendor/esp_lvgl_port/idf_component.yml"
does not point to a directory.
```

- 原因: lock の `path:` が旧パス (3 か所)。`vendor/*/idf_component.yml` は相対 (`components/...`) なので、
  lock さえ直れば通る。
- 本タスクは書き込みスコープ外なので、**検証中だけ** `dependencies.lock` のパスを現行に
  置換してビルドし、**終了後に元の内容へ復元した** (`git status` clean を確認済み)。
- 恒久対応案: `git rm --cached firmware/dependencies.lock` して lock を git 管理外にする、
  または現パスで再生成した lock をコミットする。**Lead の判断でお願いしたい**。

### 8.4 実機で確認すること (TODO(hw))

1. Kconfig on で書き込み、起動ログに `muse_link: phase1: host=... token=unset (len 0)` が出る。
2. 資格情報を NVS に入れた場合のログ (token の値が出ないこと)。
3. `int_free` が on でどれだけ減るか (`b` ベンチ)。オフ時と比較。
4. 時計・UI・BLE・電池が off 時と同等であること。

---

## 9. Phase 2 ① 資格情報の投入 (T52 実装)

Muse の資格情報 (host / vm / token) を **投入・確認・消去**できるようにした。
M1 (実アカウント) が無くても実装・ビルド・パース検証ができる部分。

### 9.1 追加したもの

| ファイル | 内容 |
|---|---|
| `src/muse_link.cpp` | `muse_link_provision` / `muse_link_clear` / `muse_link_get_status` / `muse_link_console_line`。NVS 読み書きとキャッシュ更新 |
| `src/muse_console_parse.c` / `.h` | `muse ...` 行のパーサ (純 C・ESP-IDF 非依存。ホストテスト対象) |
| `src/muse_link_internal.h` | (Phase 1 から) SDK アダプタへのキャッシュ公開 |
| `include/muse_link/muse_link.h` | 上記 API の宣言と `MUSE_LINK_TOKEN_MAX` (1023) |
| `test/test_muse_console.c` / `run.sh` | パーサのホストテスト |
| `firmware/components/watch_app/src/watch_app.cpp` | serial_dbg_task に `M` 行モード (改行まで蓄積 → `muse_link_console_line`) |

### 9.2 シリアル手順 (行モード)

USB-Serial/JTAG のモニタで **大文字 `M`** を送ると行モードに入る
(小文字 `m` は従来どおり More へ遷移。**Kconfig off では `M` も従来どおり More**)。

```
M
muse status
muse set host hatch.metaaivm.com
muse set vm <vm-id>
muse set token <token>
muse clear
```

| 仕様 | 値 / 挙動 |
|---|---|
| 行の終端 | CR / LF。行モードは改行で抜ける |
| 最大長 | 1100B/行 (token 1023B + コマンド語 15B + 余白) |
| タイムアウト | 30 秒入力が無ければ行モードを抜ける (1 文字コマンドが死んだままにならない) |
| バックスペース | 0x08 / 0x7F で 1 文字削除 |
| ログ | **行の内容は出さない** (token を含みうる)。長さだけ出す |
| 値なし | `muse set host` のように値を省くと**その項目だけ消す** |
| 反映 | 即時 (NVS 保存 + メモリ内キャッシュ更新)。再起動不要 |
| 無効時 | `CONFIG_WATCH_ENABLE_MUSE_LINK=n` では行モードのコード自体がコンパイルされない |

`muse status` の出力 (token は長さのみ):

```
{"configured":true,"ready":false,"wifi":false,"host":"hatch.metaaivm.com","vm":"...","token_len":42}
```

### 9.3 保存 → 確認の流れ (コード上の経路)

1. `muse_link_console_line()` が `muse_console_parse()` でコマンドを判定
2. `muse_link_provision()` が **全項目を先に検証** (長さ・印字可能な非空白 ASCII)。
   1 つでも不正なら**何も書かない**
3. `nvs_open("muse", NVS_READWRITE)` → `nvs_set_str` / `nvs_erase_key` → `nvs_commit`
4. メモリ内キャッシュ (`s_host` / `s_vm` / `s_token`) を NVS から再読込
5. ログは host/vm の値と token の**長さだけ**
6. `muse status` は `muse_link_get_status()` の JSON (token 値なし)
7. 再起動しても `muse_link_init()` が同じキーを読む (実機確認は §9.7)

### 9.4 サイズ実測 (T52 後)

T50 Phase 1 の値 (§3) との比較。同じツリー・同じ sdkconfig で
`CONFIG_WATCH_ENABLE_MUSE_LINK` だけを変えてビルド。

| 指標 | off (既定) | T50 on | **T52 on** | T52 差分 | off から累計 |
|---|---:|---:|---:|---:|---:|
| `sas_watch.bin` | 2,184,064B (0x215380) | 2,281,312B (0x22cf60) | **2,284,880B (0x22dd50)** | **+3,568B** | +100,816B |
| Flash Code | 1,459,840 | 1,546,048 | 1,548,592 | +2,544 | +88,752 |
| .rodata | 539,236 | 548,076 | 549,100 | +1,024 | +9,864 |
| 静的 PSRAM (.ext_ram.bss) | 55,352 | 60,056 | **63,200** | +3,144 | +7,848 |
| **静的 DIRAM (内蔵)** | **321,271 (空き 20,489)** | 327,047 (空き 14,713) | **326,039 (空き 15,721)** | **−1,008** | +4,768 |
| IRAM | 16,384 (100%) | 16,384 (100%) | 16,384 (100%) | 0 | 0 |

T52 で増えた主なもの: `muse_link.cpp` 4,276B (うち code 2,096) / `muse_console_parse.c` 466B /
watch_app の行モード (~1.3KB code)。DIRAM が **T50 より 1,008B 減った**のは、
token キャッシュ (1024B)・コンソール値 (1024B)・行バッファ (1100B) を
`EXT_RAM_BSS_ATTR` で **PSRAM に置いた**ため (内蔵 SRAM は常に逼迫: AGENTS.md)。

### 9.5 検証結果

| 検証 | 結果 |
|---|---|
| 既定 off ビルド | ✅ `0x215380` (2,184,064B) **不変** |
| on ビルド | ✅ 0x22dd50。undefined reference なし |
| パーサのホストテスト | ✅ `firmware/components/muse_link/test/run.sh` → **all passed** (22 ケース + 1023B token)。`cc -std=c99 -Wall -Wextra -Werror` |
| token の漏えい | ✅ token 値を出すログ・コード・成果物なし (ログは長さのみ / 行内容も非出力)。実トークンは未使用 |
| NVS の保存/読出し | コードレビューで経路を確認 (§9.3)。実機/QEMU が無いため**実行は未確認** (§9.7) |
| core / sim | 影響なし (core 273/273、sim 監査 0 件は T50 で確認済み。T52 は firmware のみ変更) |

ビルド時の注意: `tools/fix_deps_lock.sh` を実行してから build し、終了後に
`git checkout -- firmware/dependencies.lock` で戻す (T51 で追加)。
本タスクもこの手順で検証し、lock は元に戻した (git クリーン)。

### 9.6 セキュリティ注意

| # | 注意 | 対応 / 現状 |
|---|---|---|
| S1 | **NVS 平文** | `muse/token` は平文。物理アクセスで読める。SDK も NVS 暗号化を推奨 (`esp32/README.md:445-450`)。**未導入**。Phase 2 以降で NVS 暗号化 (HMAC eFuse) かアプリ層暗号化を検討 |
| S2 | `muse clear` の限界 | NVS の erase はページ GC まで旧値が flash に残り得る。完全消去は `nvs_flash_erase` / 暗号化前提 |
| S3 | シリアルの信頼モデル | USB-Serial/JTAG を挿せる人は物理アクセス権を持つ (=既存の `b`/`s` 等と同じ)。リリースは Kconfig off が既定 (追加コード 0B) |
| S4 | ログへの漏えい | token 値は一切出さない (長さだけ)。行モードも内容を出さない。デバッグ時に値を貼らない |
| S5 | SDK NVS との互換 | キー名は SDK と同じ (`host`/`vm`/`token`) だが、**パーティション配置が違う**ので SDK の NVS イメージを SAS-Watch に書かない (§10) |
| S6 | トークンの寿命 | device/VM トークンの寿命・更新方法は M1 で確認する (§10-5)。失効時は再ペアリング/再投入 |

### 9.7 実機未確認 (TODO(hw))

1. 行モードの実機挙動 (USB-Serial/JTAG での 1023B 入力、30 秒タイムアウト、バックスペース)。
2. NVS 保存 → 再起動 → `muse status` で長さが残ること。コード上の経路は §9.3 のとおり。
3. on ビルドで `int_free` がどこまで減るか (static 空き 15,721B。`b` ベンチ手順は docs/perf.md)。
4. 大文字 `M` が行モード、小文字 `m` が More のままであること。
5. NVS 暗号化は未導入 (§9.6 S1)。

---

## 10. M1 手順の器 (実値は書かない)

M1 = 資格情報を外部から調達して B-min の接続を検証するまでの手順。
より詳しい作業手順は `docs/muse-m1.md` (並行タスクで整備) にもある。
**トークン値・VM ID の実値は、この文書・リポジトリ・チャット・ログのどこにも書かない。**

前提: M0 の公式ファーム書き込み前に SAS-Watch の 32MB 全量バックアップを取る
(`docs/muse-port.md` §9.4 M0)。

1. **M0**: 公式 Muse ファームを書き込み、Muse アプリでペアリングする。
   日本語の音声ノート → 字幕が動くことを確認。
2. **NVS ダンプ**: SDK の `esp32/partitions_muse.csv` では `nvs` は
   **offset 0x11000 / size 0xC000**。

   ```sh
   python -m esptool --chip esp32s3 -p <PORT> read-flash 0x11000 0xC000 <nvs_sdk.bin>
   ```

   ダンプはホスト側の一時領域に置き、リポジトリに入れない。読み終わったら消す。
3. **NVS から値を読む**: namespace `muse` の `host` / `vm` / `token`。
   NVS の形式は IDF の `nvs_partition_tool` / `nvs_partition_gen.py` と同じ。
   予定: `tools/muse-gadget/export_credentials.py` のようなヘルパーで
   「値を画面に出さず、SAS-Watch 投入用の行だけを作る」形にする (**未実装**、Phase 2)。
4. **SAS-Watch へ投入**: シリアルで `M` → 次の 3 行 → `muse status` で
   `token_len` だけ確認 → 再起動して再確認。

   ```
   M
   muse set host <host>
   muse set vm <vm>
   muse set token <token>
   muse status
   ```

5. **接続検証 (M1 の GO/NO-GO)**: Phase 2 後半のセッション実装後、
   `muse_link_ready()` + 実接続で音声ノート/字幕を確認する。
   トークンが通らなければ、資格情報の入手方法 (device token の寿命・
   VM token 直接利用) を再検討する。

注意:

- SAS-Watch の NVS は独自配置 (`firmware/partitions.csv`: nvs @ 0x9000 / size 0x6000)。
  **SDK の NVS イメージをそのまま書き込まない** (SAS-Watch の設定が壊れる)。
  移すのはキーと値だけ (§9.2 のコマンド)。
- 取得した値は Muse アカウントに紐づく資格情報。漏えいしたら失効・再ペアリング。
- M1 の結果 (可否・寿命・更新方法・NVS 再投入の要否) は、値抜きで本文書に追記する。
