# lightweight.md — 時計ファーム軽量化監査 (T41, 2026-11-17)

ユーザーの課題「一生の課題である軽量化」に対する、現状の**実測/静的解析**と
**優先順位付き改善提案**。コードは原則変更しない (実装は T43 で別途)。
関連: `docs/handoff.md` (実機確定値) / `docs/perf.md` (描画ベンチ) /
`docs/roadmap.md` T24/T26/T29/T32 / `docs/plan.md` L章 (電源) / `docs/board.md`。

## 0. 結論 (先に読む)

| 項目 | 現状 | いちばん効く手 |
|---|---|---|
| app (flash) | **2,183,984 B / 4 MiB = 空き 47.9%** (T43 後) | 内蔵文字盤フォントの**グリフ絞り込み (T43)**。font_fc 862,119→**123,347B (−738,772B)**、app 空き 30.4→**47.9%** を最終 build で実測 |
| 内蔵 SRAM | 静的 320,223 / 341,760 B (**空き 21,537 B**)、実行時 `int_free` 18〜21KB | **Theme の 2 コピー 47KB** と **theme_store の 51KB** を PSRAM へ (要検証) |
| PSRAM | 描画バッファ 402KiB + 画面ブロック 1.25MiB + binfont 1.4MB + スキンキャッシュ ≤512KB + アリーナ | 肌テーマの残り ~750KB が上限。断片化対策が先 |
| 描画 | standard full 40〜68ms / cute・cyber 80〜185ms | bake 851KB の**チャンク化**とスライダー塗り/縁ストリップの部分キャッシュ |
| 電池 | **消費電流は未計測** (board.md 未確認)。light sleep はコード上**完全に無効** | まず実測。そのうえで light sleep 復活 (BLE 接続ポリシーとセット) |

主張の区別: **実測** = ビルド成果物・実機ログ・手元で再計算した値 /
**静的** = ソース・設定の読み取り / **推測** = 上記からの見積り。
「体感」だけの根拠は無い (AGENTS.md)。

## 1. 計測条件 (再現用)

| 項目 | 値 |
|---|---|
| git | `1bda7de` (Merge PR #40, 2026-10-29 時点の origin/main) |
| firmware build (before) | `firmware/build` 2026-10-29 15:29 (T38・T43 の変更**前**) |
| app bin (before) | `firmware/build/sas_watch.bin` = 2,917,104 B (0x2c82f0) |
| firmware build (after T43) | 2026-11-17 12:25 / T38 完了後の最終 build |
| app bin (after T43) | 2,183,984 B (0x215330) |
| toolchain | ESP-IDF 5.5.1 / LVGL 9.5.0 / esp_lvgl_port 2.9.0 (vendor) / NimBLE |
| size 解析 | `.toolchain/espressif/python_env/idf5.5_py3.14_env/bin/python .toolchain/esp-idf/tools/idf_size.py --archives build/sas_watch.map` (読み取りのみ) |
| 実機描画値 | `docs/perf.md` の `b` ベンチ (2026-10-19〜24 実機) |
| 実機メモリ値 | `docs/handoff.md` §実機で確定した値 (2026-10-29 実機) |

> **注意**: T38 (T38 = 時計側 dictation) が 2026-11-17 に firmware/core を変更中。
> 本書の数値は T38 前のビルド。T38 マージ後に `idf.py size-components` と `b` を
> 取り直し、差分を本書に追記すること。

> **注意 (`size-components` の二重計上)**: この toolchain の
> `idf.py size-components` は map の merged `.rodata.str1.4` 群を
> 「先頭の object に群サイズ、子 object に個別サイズ」で二重計上する。
> per-archive 合計は before 3,109,115 B / after 2,376,988 B で、実 image
> (before 2,916,971 / after 2,183,847 B) より **192,144 / 193,141 B 大きい**。
> 内訳のうち `libesp_app_format.a` の 165,260 B はほぼ全部この artifact
> (実体は `.rodata_desc` の 256 B + 600B 程度)。**下の表はこの artifact を
> 補正した概算**で、合計は image サイズに合わせていない (丸め誤差あり)。

## 2. flash (app 4MiB)

### 2.1 現状 (実測)

- app bin **2,917,104 B** / app パーティション 4,194,304 B → **空き 1,277,200 B (30.4%)**。
  `firmware/build/sas_watch.bin` と `check_sizes.py` の出力
  (`.toolchain/fw.log`: `0x2c82f0 bytes ... 0x137d10 bytes (30%) free`)。
  handoff.md の「31%」は T24 直後の値で、現在のビルドは 30.4%。
- ELF 内訳 (`idf_size.py`): Flash Code 1,455,056 B + Flash Data 1,332,668 B
  = 2,787,724 B (残りはヘッダ等)。.rodata が 1,277,060 B と大きい。
- bootloader: 0x57a0 = 22,432 B / 32,768 B (空き 10,336 B = 32%)。
- **空き 1.22MiB は「機能追加の余地」であって、軽量化の余地ではない**。
  今すぐ消せる無駄が下記のフォントに集中している。
- **T43 後 (2026-11-17 最終 build)**: app bin 2,183,984 B → **空き 2,010,320 B
  (47.9%)**。ELF 合計 2,183,847 B (.rodata 539,156 B)。詳細は §2.6。

### 2.2 内訳 (before = 2026-10-29 build、size-components、merged-section artifact 補正の概算)

| 区分 | サイズ | 比率 | 主な中身 |
|---|---:|---:|---|
| libui (アプリ UI) | 1,098,754 | 37.7% | **内蔵文字盤フォント 862,119** + 画面/部品/テーマ実装 ~236KB |
| LVGL | 400,394 | 13.7% | draw_sw blend 各種・widget・default theme・lodepng 31,733 |
| WiFi + LWIP | 388,686 | 13.3% | net80211 141,073 / lwip 96,086 / wpa_supplicant 63,160 / pp 64,225 |
| TLS + HTTP + OTA | 230,433 | 7.9% | mbedtls 系 4 lib 190,784 + esp-tls/http_client/http_parser/esp_https_ota 等 39,649 (cert bundle 68,987 は下の「その他」) |
| Bluetooth (NimBLE+ctrl) | 187,655 | 6.4% | libbt 92,343 / btdm_app 91,900 |
| 自前コード (ui 以外) | 194,713 | 6.7% | watch_app/board/audio/ble_link/theme_store/ota/diag |
| ESP-IDF core/drivers | ~307,000 | 10.5% | esp_hw_support/hal/spi_flash/heap/newlib/libc 等 (artifact 補正後) |
| その他 (x509 bundle 含む) | ~136,600 | 4.7% | — |

> 補正後も各区分の合計は image より約 27.5KB 多い (残りの merged-section
> artifact と丸め)。比率は before image 2,916,971B に対する概算として読む。
> T43 後は libui が 1,098,754 → 360,484 B (フォント分 −738,270)、
> 全体 image は 2,183,847 B (§2.6)。

### 2.3 削減候補 (KB 見積り・リスク・検証方法)

| # | 候補 | 見積り | 根拠 | リスク | 検証方法 |
|---|---|---:|---|---|---|
| F1 | **内蔵文字盤フォントのグリフ絞り込み (実装済 T43)** | **−738,772B 実測** (862,119→123,347B) | `font_fc_*` 26 個 = 862,119B。`faces/` の `digits()`/`digits_thin()` は数字・`:`・`-` しか描かない (全 call site: bold:114/117, hud:143, minimal:153, chara_side:133/140/155, chara_bubble:234, analog:155/175/124)。生成済み C の glyph_dsc から bitmap 実測 (tools/lightweight/font_subset_estimate.py) | 低。未使用グリフを落とすだけ。将来 `digits()` に英字を描かせると豆腐 | build 差分 + sim 監査 + 実機の数字盤 |
| F2 | cert bundle を FULL→必要ルートのみ | −60〜65KB | `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y` (sdkconfig.defaults:139) で x509_crt_bundle 68,987B。OTA は `esp_crt_bundle_attach` (ota.cpp:127) で url は電話が指定 | 中。OTA 先の証明書ルートを絞ると将来のサーバ変更で OTA 失敗 | `ota.start` で実サーバへ 1 回 |
| F3 | ログを INFO→WARN | −10〜40KB (要計測) | `CONFIG_LOG_DEFAULT_LEVEL_INFO=y` + 多数の `ESP_LOGI` 文字列。実測は未 | 低。障害解析がやや不便 | build 差分 |
| F4 | WiFi 側の未使用機能 OFF (WPA3/SAE, IPv6, WPA3-OWE) | −30〜60KB (要計測) | sdkconfig: `ESP_WIFI_ENABLE_WPA3_SAE/SAE_PK/SAE_H2E/OWE`=y, `LWIP_IPV6=y`。自宅/テザリングは WPA2-PSK 想定 | 中。WPA3 専用 AP で OTA 不可 | build 差分 + 実 AP で OTA |
| F5 | NimBLE の未使用サービス OFF + Coded PHY OFF | −5〜15KB (要計測) | sdkconfig: BAS/DIS/HTP/IAS/LLS/SPS/ANS/CTS/PROX/HR が y (自前 GATT のみ使用)、`LL_CFG_FEAT_LE_CODED_PHY=y` | 低〜中 (汎用 BLE 機器として見えなくなる) | build 差分 + 接続/再接続 |
| F6 | WiFi+OTA スタックごと無効化 (機能判断) | **−619KB** | 上記 WiFi 388,686 + TLS/HTTP/OTA 230,433。OTA を USB 書き込み運用にする場合のみ | **高**。`ota.start` 廃止 = 製品仕様変更。ユーザー判断 | 機能合意 + build |
| F7 | コンパイル最適化 `-Os` / LTO | −100〜200KB (推測、要計測) | `CONFIG_COMPILER_OPTIMIZATION_PERF=y` (sdkconfig.defaults)。描画コードの速度と引き換え | 中。描画 `b` が悪化しうる | `b` before/after 必須 |
| F8 | LVGL 未使用 widget / THEME_DEFAULT 削減 | −20〜60KB (推測) | sdkconfig の LV_USE_* は chart/textarea/table/roller/tabview 等が全部 y。ただし `lv_theme_default.c` が全 widget の style を触るため link される。プロジェクトは自前 Theme で、`lv_theme_default_init` の使用は無い | 中。既定 style が変わる箇所が出る | sim 全画面差分 + build 差分 |
| — | 日本語フォント (jp_20/26/32, binfont) を assets へ | 済 (T24) | 862KB のうち**含まない** (font_fc_* は数字盤専用の別フォント) | — | — |
| — | montserrat 16/24/48 の無効化 | ≈0 | sdkconfig は y だが、参照が無く GC 済み (map に `lv_font_montserrat_14` 13,649B のみ) | — | — |
| — | lodepng 31,733B の削除 | 不可 (据え置き) | `png_stream` で扱えない PNG のフォールバック (docs/perf.md) | — | — |

**F1 の内訳 (実測、26 フォント合計)**: bitmap 791,177 B のうち、実使用 (数字 + `:` + `-`) に余裕を足した
16 グリフ (space/%/./-/0-9/:) でも 138,632 B しか使わない → bitmap −652,545 B、glyph_dsc 8B×落とす
グリフ数でさらに −16,392 B。**合計 668,937 B = 653.3 KiB** (さらにカーニング表の縮小分)。

**実装 (T43, 2026-11-17)**: `FACE_FONT_RANGE` を `0x2D-0x3A` (14 グリフ = `- . / 0-9 :`) に変更し、
tools/gen_fonts.py --faces で 26 個を再生成。**sim は before/after 75 画面で差分 0**
(tools/lightweight/sim_font_diff.py、監査 0 件)。
**コンパイル後の実測**: T38 の作業中ビルド (2026-11-17 12:12、サブセット版フォントを
含む) の map で `font_fc_*` 26 個の合計は **862,119 → 123,347 B (−738,772 B)**。
T38 のコード変更を含む app bin は 2,917,104 → 2,183,984 B、app 空き 47.9%
(ただし T38 完了後の最終 build で再確認する。§2.5 と §9 参照)。

### 2.4 パーティションの余地 (静的)

32MiB flash の実配置 (`build/partition_table/partition-table.bin` を decode):

```
nvs 0x9000/24K, otadata 0xf000/8K, phy 0x11000/4K,
ota_0 0x20000/4M, ota_1 0x420000/4M,
assets 0x820000/16M, storage 0x1820000/6M, coredump 0x1e20000/64K
→ 末尾に 0x1d0000 = 1,900,544 B (~1.81MiB) の予備
```

- app は OTA 2 面 4MiB ずつ。**予備 1.81MiB を使って 1 面あたり最大 +0.9MiB まで
  広げられる** (ただし T24 の「パーティション拡張余地は未確認」への回答。
  拡張するなら assets/storage を削るか予備を使う。**軽量化の本筋ではない**)。
- assets 16MiB / storage 6MiB は現状で十分な余裕 (テーマ 4 個 + binfont 1.4MB)。

### 2.5 T43: 文字盤フォントのサブセット化 (実装メモ)

- 対象: `firmware/components/ui/src/fonts/font_fc_*.c` 26 個 (5 family × 5〜6 size)。
  `tools/gen_fonts.py --faces` が TTF から生成する。従来は `FACE_FONT_RANGE=0x20-0x7E`
  (ASCII 95 グリフ) を焼いていたが、`faces/` の `digits()`/`digits_thin()` の
  呼び出しは数字・記号しか描かないため **`0x2D-0x3A` (14 グリフ)** に絞った。
  空白は数字ラベルに現れないので含めない。英字を `digits()` で描くと豆腐になるので、
  その時は `--face-chars` で範囲を広げて再生成する。
- **落とし穴 1: line_height / base_line**。lv_font_conv は「含めたグリフの最大高さ」
  から両値を計算するため、絞ると oswald 150 で 165/29 → 126/3 に変わる。LVGL は
  この 2 値でラベル内ベースラインを決めるので、そのままだと数字が十数 px ずれる。
  `FACE_FONT_METRICS` (ASCII 全量時代の値) を生成後に書き戻して一致させた。
- **落とし穴 2: カーニング**。lv_font_conv はグリフ集合に応じて kern_scale と
  形式 (pairs/classes) を選び直すため、数字ペアの送り幅が 1/16px 単位で変わる。
  中央寄せラベルの幅の丸めが変わり、数字が 1px 動く (sim 差分で 3 画面に出た)。
  `FACE_FONT_KERN` (当時の kvalue と kern_scale を残したグリフの組だけ固定) を
  書き戻し、LVGL の `(kvalue * kern_scale) >> 4` を当時と一致させて 0 差分にした。
- 生成は決定的 (同じ TTF と gen_fonts.py から 2 回生成して sha256 一致を確認)。
  再生成は `python3 tools/gen_fonts.py --faces` (lv_font_conv 1.5.3+。
  PATH に無ければ `.toolchain/npm-cache` か `~/.npm/_npx` のキャッシュを使う)。
- 検証: sim snapshot 75 画面差分 0 / 監査 (safe-area・overlap・glyph) 0 件。
  **コンパイル後**: 26 フォント合計 862,119 → 123,347 B (−738,772 B、−85.7%) を
  T38 の作業中ビルドの map で実測 (font_fc 以外の差は T38 の変更なので、
  app 全体の空きは T38 完了後の最終 build で確定する)。
  core テストと firmware build は T38 完了後 (§7 手順)。
- 参考: T3 で入れた「数字の等幅化」は T16 の範囲変更 (0x20-0x7E) 以降
  グリフ添字がずれて実際には効いていないことが判明 (現行フォントの数字は
  フォント本来の送り幅)。等幅化すると数字の見た目が変わるため T43 では保留し、
  別タスク (T44 候補) で sim 差分を見て判断する。tools/gen_fonts.py に TODO を残した。

### 2.6 T43 最終結果 (before/after 実測)

計測は `idf.py build` / `idf.py size` / `idf.py size-components` と、ビルド済み
map の per-file 集計 (tools/lightweight/flash_report.sh)。before は 2026-10-29
の build (T38・T43 前)、after は 2026-11-17 12:25 の最終 build (T38 完了後・
T43 フォント反映)。**after には T38 のコード変更も含む**ため、フォント単体の
効果は font_fc の行で分離して示す。

| 指標 | before | after | 差 |
|---|---:|---:|---:|
| app bin | 2,917,104 B | **2,183,984 B** | **−733,120 B (−17.5%)** |
| app 空き | 1,277,200 B (30.4%) | **2,010,320 B (47.9%)** | **+733,120 B (+17.5pt)** |
| ELF 合計 | 2,916,971 B | 2,183,847 B | −733,124 B |
| .rodata | 1,277,060 B | 539,156 B | −737,904 B |
| **font_fc_\* 26 個 (per-file)** | **862,119 B** | **123,347 B** | **−738,772 B (−85.7%)** |
| libui.a (size-components) | 1,098,754 B | 360,484 B | −738,270 B |
| 内蔵 SRAM (静的 DIRAM) | 320,223 B (空き 21,537) | 321,271 B (空き 20,489) | −1,048 B (T38 の追加) |
| IRAM | 16,384 / 16,384 (100%) | 同左 | 0 |

- **フォント単体の削減は −738,772 B**。app 全体の −733,120 B は
  T38 の新機能 (dictation) の +5,652 B を含むため、差引で整合する。
- 空き 47.9% は目標 (~45%) を上回った。**これで app 4MiB の半分が空く**。
- 検証: `idf.py build` 成功 / core テスト **273/273 pass** /
  sim snapshot **監査 0 件 (safe-area・overlap・glyph)** /
  sim before/after **75 画面 差分 0** (tools/lightweight/sim_font_diff.py)。
- 未実施: 実機の `b` ベンチと数字盤の目視。グリフ数が減ったことで
  LVGL のフォントキャッシュ挙動が変わりうるため、書き込み後に
  `b` の before/after (少なくとも standard/cute/cyber の Home) を取って
  docs/perf.md へ記録するのが望ましい (§7.2、AGENTS.md)。
- 参考: 減った flash は OTA 2 面それぞれに効く。assets (16MiB) には
  日本語 binfont 1.4MB があるため、フォントを assets へ移す案 (F1 の別案)
  は不要になった (PSRAM 予算を増やさずに済む)。

## 3. 内蔵 SRAM / PSRAM

### 3.1 静的内訳 (実測、ELF セクション)

| メモリ | 使用 | 合計 | 空き |
|---|---:|---:|---:|
| DIRAM (内蔵 SRAM 静的) | 320,223 | 341,760 | **21,537 (6.3%)** |
| ├ .bss | 152,208 | | |
| ├ .text | 117,771 | | |
| └ .data | 50,244 | | |
| IRAM | 16,384 | 16,384 | **0 (100%)** |
| RTC SLOW / FAST | 148 / 80 | 8,192 各 | 8,044 / 8,112 |

- IRAM は満杯。**IRAM を食う変更 (fast_mem/ISR 属性) は追加できない**。
- 実行時の内蔵ヒープ空きは `b` の `int_free` = **18〜21KB** (handoff.md 実測)。
  静的 21.5KB と整合。BLE init は 32KB 連続を要求するため、**この 20KB が
  ブート成否の余白** (sdkconfig.defaults:40 の `SPIRAM_MALLOC_ALWAYSINTERNAL=1024`
  でしのいでいる)。

### 3.2 内蔵 SRAM の常駐物 (実測: `size-components` + `nm`)

| 消費 | サイズ | 正体 / 根拠 |
|---|---:|---|
| `ui` の Theme コピー | **47,280** | `Theme` 1 個 = 11,820B (nm: 0x2e2c)。`s_base`/`s_active` (theme.cpp:109-110) + `kStandard`/`kLight` (theme.cpp:30/69) = 4 個が DRAM。**単一で最大の無駄**。別に PSRAM 側にも `theme_apply::t` / `load_file_theme::t` の 2 個 (theme_manager.cpp:135/244) がある |
| `theme_store` | **51,101** | `mbuf[32*1024]` (theme_store.cpp:77) + `ThemeManifest m` 12,268B (theme_store.cpp:84) + 一覧/zip 構造体 ~7KB。インストール時しか使わない |
| `watch_core` | 28,184 | プロトコル受信/設定/NVS ステージング等 |
| FreeRTOS | 20,598 | idle/timer タスク、キュー、静的スタック |
| `watch_app` | 20,576 | 自分のタスク 8KB スタック (静的)、設定コピー等 |
| BLE controller (btdm_app) | 18,116 | ベンダ lib (削れない) |
| memo (`g_entries`) | 17,408 | メモ 500 件のキャッシュ (apps/memo/feature/memo.cpp:50) |
| WiFi `pp` | 17,058 | WiFi プロトコル処理 (接続時のみ使用) |
| hal / esp_hw_support / spi_flash / phy | 50,000 前後 | ベンダ/IDF (削れない) |

### 3.3 PSRAM の常駐物

| 常駐物 | サイズ | 根拠 |
|---|---:|---|
| LVGL 描画バッファ (全面 410×502, RGB565) | **402KiB** (411,640B) | esp_display.cpp:445-455。失敗時は 252/126 行に縮小 |
| 画面ブロック (bg/deco デコード先) | **640KiB × 2 = 1.25MiB** | ui/port.hpp:89 `kThemeScreenBlockSize`、esp_port.cpp:112 (肌テーマのみ確保) |
| 日本語 binfont (jp_20/26/32) | **1.37MiB** (1,437,744B) | tools/fonts/bin の合計。`ui::init_display` で PSRAM へロード (esp_display.cpp:508-515、T24) |
| スキンキャッシュ (9-slice 合成) | **≤512KiB** | components.cpp:534-540 (`kSkinCacheBytes`) |
| テーマアリーナ (資産領域) | **見積り分、上限 3MiB** | esp_port.cpp:90-92 (`kAssetCap`)。内蔵テーマは 0 |
| PSRAM BSS (Theme/ThemeManifest) | ~35.9KB | theme_manager.cpp:121/135 の `SAS_BIG_STATIC` |
| LVGL ヒープ (widget/style/描画レイヤ) | 可変 | `lv_mem_psram.c` で `heap_caps_malloc(SPIRAM)` (T29) |

**実機の PSRAM 空き (handoff.md 実測)**: standard ~3.4MB (アリーナ無し) /
肌テーマ (cute/cyber) ~750KB / mame ~450KB。最大連続ブロックは肌テーマで ~500KB。
→ **PSRAM は「空き」より「連続ブロック」が制約**。新しい常駐を足すときは
全テーマで `b` の `psram_free` / `psram_largest` を確認 (AGENTS.md)。

### 3.4 メモリ改善候補

| # | 候補 | 効果 | リスク | 検証 |
|---|---|---|---:|---|
| M1 | Theme を PSRAM 化 + 定義側を constexpr 化 | 内蔵 SRAM **+47KB** (s_base/s_active を PSRAM、kStandard/kLight を flash rodata 化)。BLE 32KB 連続の余白が増え、`PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP=y` (現状 n、sdkconfig.defaults:99) に戻す前提が整う | 中。`theme()` は widget 生成時に読む。描画ループで読む箇所は要確認 | `b` 全画面 + 起動 10 回 |
| M2 | theme_store の 51KB を PSRAM 化 | 内蔵 SRAM **+51KB**。`mbuf` 32KB は zip 読取の一時領域、`ThemeManifest` は 11.7KB の大構造体 | 低〜中。BULK/テーマ install 経路 (T25) の挙動確認 | テーマ適用 + `b` |
| M3 | memo `g_entries` 17KB を PSRAM 化 | +17KB | 中。core は platform 非依存なので PSRAM 属性を直接使えない (interface 追加が要る) | メモ一覧 + core テスト |
| M4 | bake バッファのチャンク化 | PSRAM 連続要件 851KB → 2〜3 分割 (各 ~280〜425KB) | 低。見た目は同一 (境界は画像を並べるだけ) | sim 差分 + `b` |
| — | LVGL 描画バッファ 2 枚化 | 不可 (PSRAM +402KiB。perf.md で棄却済み) | — | — |
| — | binfont を都度ロードに | 不可 (起動時プリロードが前提。T24 の OOM 対策) | — | — |

## 4. 描画 (再点検)

### 4.1 現在の before/after (docs/perf.md の再整理、単位 ms・実機)

| 画面 | standard full | cute full | cyber full | scroll (テーマ別) |
|---|---:|---:|---:|---|
| Home | 40 | 80 | 85 | — |
| アプリ一覧 | 68 | 117 | 120 | 70 (standard) / **~180 (cute/cyber、bake 失敗時)** |
| 設定 | 56 | 141 | 157 | 74 (standard) / 167 (cute) / 185 (cyber) |
| ストップウォッチ | 44 | 98 | 103 | — |
| タイマー | 55 | 133 | 144 | — |
| 通知 | 44 | 92 | 96 | — |

(数値は docs/perf.md の実機 `b` ベンチ。full = 全面再描画、scroll = リストを
12px ずつ動かした 1 フレームの時間。)

- 旧パイプライン (内蔵 10 行バッファ) は standard 85〜112ms。T29 の全面 PSRAM
  バッファ化 + スキンキャッシュ + 近似塗りでここまで来た。
- スキン描画を全部止めると (`z`=3) cute/cyber も全画面 ~40ms。
  **残差 40〜110ms はほぼスキン画像の伸縮描画**。

### 4.2 まだ重い経路 (静的 + 実機 skinstats)

1. **大きいカード (346×344 等) の 9-slice** — `kSkinCacheMaxPx=32768` を超えるため
   **一生キャッシュされない** (components.cpp:540、`b` の `rej(size=69 room=0 346x344)`。
   ただし「大きい部品はキャッシュ blit より直接描画の方が速い」と実測済みで、
   単純に上限を上げるのは逆効果 — perf.md / components.cpp:534-540 のコメント)。
   → 残るコストは**縁ストリップの伸縮**と中央グラデ矩形。
2. **スライダー塗り** — 値で幅が変わるのでキャッシュしない設計
   (components.cpp:952-956、`miss` に計上)。設定画面に 3 本 + クイックに 1 本。
3. **アプリ一覧 bake 失敗時のライブ描画** — 851KB (ARGB8888) の連続確保に失敗すると
   `~180ms` のスクロールにフォールバック (T24/T32、handoff.md)。
4. **cute/cyber の Home 80〜85ms** — 画面スキン (bg/縁) の伸縮。z=3 で ~40ms。

### 4.3 改善案と期待効果

| # | 施策 | 期待効果 | リスク | 検証 | 根拠 |
|---|---|---:|---|---|---|
| R1 | **bake を 2〜3 チャンクに分割** (各行 zone を別 draw_buf に snapshot し、画像を並べる) | 一覧スクロール 180ms → **~70〜120ms** 相当 (standard の 70ms + スキン分)。cute/cyber の最大の UX 改善 | 低。合計メモリは同じ、連続要件だけ下がる。境界の継ぎ目は 0px で並べる | `b` (cute/cyber 一覧 scroll) + sim 差分 + `No memory` ログ消滅 | components.cpp:1132-1178。PSRAM 最大連続 ~500KB に対し 851KB が失敗要因 |
| R2 | **縁ストリップだけキャッシュ** (9-slice を「角+辺(伸縮)+中央」に分け、辺/角だけ実表示サイズで合成。中央は現行の矩形塗り) | 設定/一覧の full を 1〜3 割減 (推測)。cute 141→~110ms など | 中。LVGL の補間結果と一致させる実装が要る (perf.md: 強引な近似は cute で max=146 の差)。**同一補間式で事前合成**なら差は出ない | `z` 帰属 + skinstats の `miss` 減 + sim 差分 (cyber 同等の max<=17 基準) | perf.md「縁ストリップは伸縮補間が見た目の一部」 |
| R3 | **スライダー fill の中央を 1D 走査で矩形塗り** (既存 `skin_1d_scan` を流用。単色/グラデなら矩形、だめなら画像のまま) | 設定 scroll 167→~150ms (推測)。3 本ぶんの miss が減る | 低。近似できない絵は画像に戻る安全設計 (perf.md) | `b` settings scroll + sim 差分 | components.cpp:952-956、perf.md の fill/band 節 |
| R4 | 描画バッファ/転送の調整 | 未計測 (full buffer 1 枚 + bounce 8.2KB×2 が既に最適化済み) | — | — | perf.md「試して棄却したもの」 |
| R5 | 画面遷移アニメの短縮 (`anim_ms`) | 体感のみ。描画回数は減るが ms 値は不変 | 低 | `b` は不変、体感確認 | theme トークン `anim_ms` |

> R1 は **bake の合計メモリが 851KB のまま**な点に注意。肌テーマの PSRAM 空き
> ~750KB では、スキンキャッシュ (512KB) を捨てた後なら 1.26MB 使えるので
> 425KB×2 は確保できる見込み。**実測で確認する**。

## 5. 電池 (設定の棚卸し)

### 5.1 現状の設定 (静的) と実測状況

| 項目 | 現在の値 | 根拠 | 実測 |
|---|---|---|---|
| BLE 接続間隔 | 100〜200ms (itvl 80〜160 ×1.25ms)、slave latency 4、supervision 5s | ble_link.cpp:66-72 | 未計測 |
| BLE 広告 | 画面 ON 500ms〜1s / 画面 OFF **1.0〜1.3s に低速化** | ble_link.cpp:60-65、723-725、power_apply.cpp:73-76 | 未計測 |
| walk の 10 秒ポーリング | ウォーク画面が Active かつ前面のときだけ `walk.query`。Dim/ScreenOff/他画面では送らない | apps/walk/feature/walk.cpp:19-20, 37-46 | 未計測 (電池影響) |
| 腕上げ起床 (WoM) | 224mg / blanking 12 (~570ms @21Hz) | board/imu.cpp:167-175 | 未計測 (取りこぼし率) |
| Dim / ScreenOff / DeepSleep | 8s / 12s / 1800s (無操作。deep は **BLE 未接続かつ lease 無し** が条件) | core/include/watch/settings.hpp:18-20、core/src/power.cpp:30-45 | 実機動作は確認済 (handoff) |
| light sleep | **完全に無効**。`light_sleep_enable=false` (CPU 240MHz 固定) + BLE 接続中は `ESP_PM_NO_LIGHT_SLEEP` lock + `PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP=n` | watch_app.cpp:584-590、ble_link.cpp:99-124、sdkconfig.defaults:99 | 未計測 (休止電流) |
| DFS (CPU 80〜160MHz) | 無効 (min=max=240MHz) | watch_app.cpp:586-589 | 未計測 |
| 画面 OFF 中 | パネル OFF + 輝度 0 + LVGL タイマー停止 (描画 0) | power_apply.cpp:61-66 | 待機中 redraw 0 は実機確認 (handoff) |
| 描画 → バス占有 | full 40〜185ms/フレーム。アイドル時は再描画 0 | docs/perf.md、handoff | 実測 (時間) / 電流は未計測 |
| マイク | 起動時に I2S RX/DMA を確保、録音時だけ codec open (クロック停止) | audio.cpp:704-810 | 未計測 (待機電流) |
| スピーカー | この個体は無効 (`WATCH_ENABLE_SPEAKER=n`)、PA は再生時のみ ON | sdkconfig.defaults、audio.cpp:735-750 | 実測済 (無効) |
| パネル/輝度 | 既定輝度 50%、Dim 20%、消灯 0 | settings.hpp:16、power_apply.cpp:29 | 未計測 |
| 消費電流 (全体) | **未計測**。plan.md の 60〜100mA / 2〜5mA / 0.1〜0.3mA は【要実機】の推測値 | docs/plan.md:561-569 | **未計測** (board.md 未確認リスト) |

### 5.2 電池に効く施策 (実測前の候補)

| # | 施策 | 期待効果 | 根拠・リスク | 検証 |
|---|---|---|---|---|
| P1 | **消費電流の実測を先に行う** | 効果の大小を決める前提。画面 ON/OFF/Dim/DeepSleep × BLE 接続有無の 6 状態 | board.md「各状態の消費電流」未確認。USB 電流計 or AXP2101 の電池電流 | 下記 §6.4 |
| P2 | **light sleep の復活** (BLE 未接続時から) | 画面 OFF の休止電流を大きく下げる (計画値 −650uA 級の可能性)。まず BLE 未接続限定なら安全 | BLE 接続中はリンクが壊れるため lock 済み。`PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP=n` は内部 SRAM 不足の回避 (sdkconfig.defaults:91-99) → **M1/M2 の SRAM 解放とセット** | `h`/`b` の復帰 + 起床/再接続テスト |
| P3 | BLE 接続間隔の延長 (例: 200〜400ms) | 接続維持電流を削減。通知遅延 +100〜300ms | 電話側の UX (通知・walk) と引き換え | `b` 不変、体感/電池実測 |
| P4 | idle 時の LVGL 周期を 33ms→100ms | アイドル wake 回数 1/3。描画は dirty 時のみなので影響小 | `LV_DEF_REFR_PERIOD` (sdkconfig.defaults:114)。アニメの滑らかさに影響 | アイドル電流 + `b` |
| P5 | 画面 OFF 時の IMU ODR を下げる | 歩数・腕上げを維持しつつサンプル数削減 | imu_service の設定次第。歩数取りこぼし | 歩数比較 |
| P6 | ログ抑制 (INFO→WARN) | シリアル出力の CPU/電力 (微小) | F3 と同根 | build 差分 |

> **注意**: 現状 light sleep が無効なので、plan.md の「画面OFF＋Light Sleep＋BLE接続
> 2〜5mA」は**達成されていない可能性が高い** (未計測)。電池持ちの議論は
> P1 の実測なしに結論を出さない (AGENTS.md)。

## 6. 優先度と「次にやる 3 件」

### 6.1 優先度表 (効果 × リスク × 検証コスト)

| 優先 | 施策 | 効果 | リスク | 検証 | 状態 |
|---|---|---|---|---|---|
| **S** | F1 フォントのグリフ絞り込み | **flash −738,772B / app 空き 30.4→47.9% 実測** | 低 | build + sim + 実機 | **T43 完了** |
| **S** | P1 消費電流の実測 (6 状態) | 以後の電池判断の土台 | 低 (測るだけ) | USB 電流計 | 未着手 |
| **A** | R1 bake のチャンク化 | 一覧スクロール 180ms → ~70〜120ms | 低 | `b` + sim | 提案 |
| **A** | M1 Theme の PSRAM/constexpr 化 | 内蔵 SRAM **+47KB** | 中 | `b` + 起動 | 提案 |
| **A** | M2 theme_store の PSRAM 化 | 内蔵 SRAM **+51KB** | 低〜中 | テーマ適用 | 提案 |
| **A** | R2 縁ストリップの部分キャッシュ | 設定 full 141→~110ms (推測) | 中 | `z` + sim | 提案 |
| **A** | R3 スライダー fill の 1D 塗り | 設定 scroll 167→~150ms (推測) | 低 | `b` + sim | 提案 |
| **A** | P2 light sleep 復活 | 休止電流 (未計測) | 高 (BLE) | 復帰/再接続 | 要 P1 + M1/M2 |
| **B** | F2 cert bundle 絞り | −60〜65KB | 中 | OTA 実機 | 提案 |
| **B** | F4 WiFi 未使用機能 OFF | −30〜60KB (要計測) | 中 | build + OTA | 提案 |
| **B** | F5 NimBLE 未使用サービス OFF | −5〜15KB (要計測) | 低 | build + 接続 | 提案 |
| **B** | F6 WiFi/OTA スタック無効化 | −619KB | **高 (製品判断)** | 合意 | ユーザー判断 |
| **B** | F3 ログ抑制 | −10〜40KB | 低 | build 差分 | 提案 |
| **B** | F7 -Os / LTO | −100〜200KB (推測) | 中 | `b` | 提案 |
| **B** | F8 LVGL widget/theme 削減 | −20〜60KB (推測) | 中 | sim 全差分 | 提案 |
| **B** | P3〜P6 電池微調整 | 未計測 | 低〜中 | 実測 | 要 P1 |

### 6.2 次にやる 3 件

1. **T43: フォントのグリフ絞り込み** (font_fc −738,772B、app 空き 47.9%)。
   実装・検証・最終 build まで完了 (§2.6)。残りは実機書き込み後の
   `b` ベンチと数字盤の目視だけ。
2. **消費電流の実測 (6 状態)**。board.md の未確認項目を埋め、
   `画面ON / Dim / 画面OFF(BLE接続/未接続) / DeepSleep` の実測表を作る。
   これ無しに「電池が持つ/持たない」を言わない。
3. **Theme + theme_store の PSRAM 化 (内蔵 SRAM +98KB)**。
   BLE init 32KB 連続の余白を作り、`PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP=y` と
   light sleep 復活 (P2) への前提を整える。`b` の `int_free` が +98KB 相当に
   なることを確認。

## 7. 実測手順 (コマンド付き)

### 7.1 flash / 静的メモリ (ビルド済み `build/` を壊さない読み取り)

```bash
cd firmware
# 既存 build/ を使ってサイズを読む (再ビルドしない)
. ../../.toolchain/esp-idf/export.sh
IDF_CCACHE_ENABLE=1 idf.py size
IDF_CCACHE_ENABLE=1 idf.py size-components > /tmp/size-components.txt
# app の空きは build ログ末尾が確実 (check_sizes.py の出力)
grep "binary size" ../../.toolchain/fw.log | tail -1
```

> どうしても IDF 環境を汚したくないときは map を直接読む:
> ```bash
> PY=.toolchain/espressif/python_env/idf5.5_py3.14_env/bin/python
> $PY .toolchain/esp-idf/tools/idf_size.py --archives firmware/build/sas_watch.map
> ```
> **`size-components` の合計は merged rodata の二重計上で実 image より
> 約 192KB 大きい** (§1 の注意)。判定は `sas_watch.bin` のサイズで行う。

### 7.2 実機の描画ベンチ (`b`) と health (`h`)

```bash
# ポートは毎回確認
ls /dev/ttyACM*
# モニタで 1 文字コマンド (b=ベンチ, h=心拍/ヒープ, t=テーマ巡回, z=スキン段階)
python -m esp_idf_monitor --port /dev/ttyACM0 firmware/build/sas_watch.elf
# → 'b' を送り、bench: / bench: skinstats: 行を docs/perf.md 形式で保存
```

自動化する場合 (DTR/RTS は触らない):

```bash
python - <<'PY'
import serial, time
p = '/dev/ttyACM0'  # ls で確認
s = serial.Serial(p, 921600, timeout=0.2)
s.write(b'b'); time.sleep(30)          # 6 画面 × full/scroll
out = s.read_all().decode('utf-8', 'replace')
print('\n'.join(l for l in out.splitlines() if 'bench' in l))
PY
```

- before/after は **同じテーマ・同じ画面**で取り、`git rev-parse --short HEAD` と
  ビルド時刻を必ず記録する。描画に効く変更は `docs/perf.md` に記録 (AGENTS.md)。
- PSRAM/SRAM の余裕は `b` 行の `int_free` / `psram_free` / `psram_largest` を
  **5 テーマ全部**で確認 (AGENTS.md のメモリ予算)。

### 7.3 sim で before/after (実機なしの見た目確認)

```bash
cmake -S sim -B sim/build && cmake --build sim/build -j
mkdir -p /tmp/sim_before
./sim/build/snapshot --out /tmp/sim_before       # 変更前のツリーで
# 変更後
./sim/build/snapshot --out /tmp/sim_after
# 差分 (同梱スクリプト。差分があれば exit 1)
python3 tools/lightweight/sim_font_diff.py /tmp/sim_before /tmp/sim_after
```

- 安全域/重なり/欠けグリフの監査は snapshot 実行時の出力 0 件を保つ。
- 文字盤の数字盤は `01_home.png` と、`t` 相当のテーマ別スクショで確認
  (T43 は faces の数字のみ変えるので、差分は数字のアンチエイリアス以外 0 が目標)。

### 7.4 消費電流 (未計測。P1 で最初にやる)

1. USB 電流計 (給電側) で、時計を USB から切り離した電池駆動の状態を測る。
2. 状態ごとに 60 秒平均を取る (シリアルは電池駆動では使えないので、
   状態遷移は PWR ボタン/無操作タイマーで作る):
   - 画面 ON ホーム / Dim (8 秒放置) / 画面 OFF (12 秒放置)
   - BLE 接続あり・なし (電話のペアリングを切る)
   - deep sleep (30 分放置 or `s` コマンド。USB 給電だと `s` が使いやすい)
3. ログに残すなら AXP2101 の電池電圧/電流を `battery_percent` と一緒に
   1 分ごとに `h` へ出す実装を足す (diag)。plan.md:569 の方針。
4. 結果は `docs/board.md` の「各状態の消費電流」と `docs/handoff.md` に記録
   (本書は T41 の提案時点のため未計測)。

## 8. 根拠ファイル一覧

| 話題 | 場所 |
|---|---|
| メモリ予算の原則 | AGENTS.md「実機で痛い目を見たので守ること」 |
| 描画パイプラインと実測 | docs/perf.md (全面バッファ / bounce / スキンキャッシュ / fill-band) |
| 実機確定値 | docs/handoff.md「実機で確定した値」 |
| T24/T26/T29/T32 の経緯 | docs/roadmap.md |
| 電源状態と見積り | docs/plan.md L章 (524-569) |
| ハード・未計測項目 | docs/board.md (60-67) |
| フォント生成 | tools/gen_fonts.py:89-119 (`FACE_FONT_SPECS` / `FACE_FONT_RANGE`) |
| 文字盤の数字描画 | firmware/components/ui/src/faces/ (faces.cpp:173-183 が入口) |
| フォント本体 | firmware/components/ui/src/fonts/font_fc_*.c (26 個) |
| Theme 定義/コピー | firmware/components/ui/src/theme.hpp:15、theme.cpp:30/69/109-110 |
| 画面ブロック / アリーナ | firmware/components/ui/include/ui/port.hpp:89、esp_port.cpp:90-152 |
| 描画バッファ | firmware/components/ui/src/esp_display.cpp:88、445-515 |
| スキンキャッシュ / bake / slider | firmware/components/ui/src/components.cpp:534-540、957-967、1132-1178 |
| BLE パラメータ | firmware/components/ble_link/src/ble_link.cpp:60-72、99-124 |
| 電源状態の適用 | firmware/components/watch_app/src/power_apply.cpp、watch_app.cpp:584-595 |
| WoM | firmware/components/board/src/imu.cpp:142-190 |
| 電池の見積り (推測値) | docs/plan.md:561-569 |
| 本監査のスクリプト | tools/lightweight/font_subset_estimate.py (削減見積り) / tools/lightweight/sim_font_diff.py (sim 差分) |
| フォント生成 (T43) | tools/gen_fonts.py の `FACE_FONT_RANGE` / `FACE_FONT_METRICS` / `FACE_FONT_KERN` |

## 9. 更新履歴

- 2026-11-17: 初版 (T41)。baseline `1bda7de` / build 2026-10-29。
- 2026-11-17: T43 実装 (§2.5)。`FACE_FONT_RANGE` を `0x2D-0x3A` に絞り 26 フォントを
  再生成。line_height/base_line とカーニングを当時の値に固定して sim 差分 0 を確認。
  T38 の作業中 build の map で font_fc 合計 862,119 → 123,347 B (−738,772 B) を実測。
  **最終 firmware build と `b` ベンチは T38 (watch-stream) の完了待ち**。
- 2026-11-17 (最終 build): T38 完了後に `idf.py build` が成功。
  app bin 2,917,104 → 2,183,984 B、空き 30.4 → **47.9%**。
  font_fc 26 個 862,119 → 123,347 B (−738,772 B) を実測 (§2.6)。
  core 273/273、sim 監査 0・差分 0。`b` ベンチと実機目視は書き込み後。
