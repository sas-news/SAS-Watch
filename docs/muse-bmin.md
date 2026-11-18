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

## 4. NVS スキーマ (書き込みは Phase 2)

namespace `muse`:

| key | 型 | 内容 |
|---|---|---|
| `host` | str | VM のホスト。未設定なら SDK 既定 `hatch.metaaivm.com` |
| `vm_id` | str | VM ID (空 = アカウント既定 VM) |
| `token` | str | device token または VM token。**ログ・UI に出さない** |

Phase 1 は起動時に `nvs_open("muse", NVS_READONLY)` で読むだけ。namespace が無ければ
「未設定」として既定値で動く (エラーにしない)。値はコード・ドキュメントに埋めていない。
書き込み経路 (BLE `settings.set` / シリアル) は Phase 2。

## 5. 公開 API と Phase 1 の挙動

`include/muse_link/muse_link.h`:

| API | Phase 1 の挙動 |
|---|---|
| `esp_err_t muse_link_init(void)` | NVS を読んでキャッシュし、`muse_link` タグで host/vm/token 有無をログ。**ネットワーク/タスク/大確保なし**。未設定でも `ESP_OK` |
| `bool muse_link_ready(void)` | 「token と host があり、Wi-Fi 接続中」。Phase 1 では Wi-Fi を張らないので実質 false |
| `void muse_link_status(muse_link_status_t*)` | 秘密を含まない状態 (host/vm_id/configred/ready/wifi) |
| `esp_err_t muse_link_chat(const char*, cb, ctx)` | **ESP_ERR_NOT_SUPPORTED** (TODO(muse): Phase 2 で `/chat/stream` + `/chat/subscribe`) |

アダプタ 20 関数の Phase 1 の値: settings は NVS キャッシュ、link は false、wifi は
`wifi::connected()`、options は全 no-op、`muse_state_page` は 20 桁 × 8 行。
未確定は `TODO(hw)` / `TODO(muse)` を付けている (`firmware/components/muse_link/README.md`)。

`muse_adapters.cpp` の `s_link_keep[]` は、Phase 1 がセッションを開始しないため
chat オブジェクトがリンカに落とされないよう参照するだけの表 (実行しない)。
TODO(muse): Phase 2 でセッション開始を実装したら削除する。

## 6. Phase 2 の残作業と工数

| # | 作業 | 見積り | 備考 |
|---|---|---:|---|
| 1 | 資格情報の投入経路 (BLE `settings.set` or シリアルで NVS `muse/*` を書く) | 0.5 日 | 既存 protocol のキー追加。token はマスク表示 |
| 2 | Wi-Fi の on-demand 制御 (`wifi::session_start/end` を「画面 ON + 送受信中」だけ) | 0.5〜1 日 | `ota.cpp:118,159` と同じ作法。電池の要 |
| 3 | セッション開始 (`muse_hatch_start` 相当) + **RAM トリム** + **実機 TLS 内部 RAM 実測** | 1.5〜2 日 | ここが実質の GO/NO-GO。素の 1.33MB PSRAM を 250〜300KB に削る。mbedTLS の PSRAM 化 or 動的バッファを決める |
| 4 | 音声アダプタ (16kHz PCM → WAV/base64、20ms チャンク) | 0.5 日 | T47 §5。スピーカー無効・字幕のみ |
| 5 | UI 統合 (`apps/agent` の partial/reply へ、15 秒上限、960B ページング、日本語エラー) | 1〜1.5 日 | T47 §8。core/protocol の変更は不要 |
| 6 | 回帰 (core/sim/firmware、`b` ベンチ全テーマ、`int_free`/`psram_free`) | 1 日 | AGENTS のメモリ/描画ルール |
| | **合計** | **4〜6 人日** | T49 の B-min 4〜6 人日から Phase 1 (0.5 日) を引いた残りと整合 |

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
