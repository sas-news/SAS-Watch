# AGENTS.md

## 最初に読むもの

| ファイル | 中身 |
|---|---|
| `docs/handoff.md` | **現状** (実機で確定した値、未解決の問題、実機作業の注意) |
| `docs/roadmap.md` | 残タスクと決定済みの方針。ここの順・方針どおりに進め、各タスクの `状態:` を更新する |
| `docs/plan.md` | 設計 (アーキテクチャ・Feature・UI・電源・保存) |
| `docs/board.md` | ハード (部品・GPIO・電源レール) |
| `docs/protocol-v1.md` | BLE 仕様 (method 一覧はこの表が唯一の正) |
| `docs/perf.md` | 描画パイプラインと性能計測 (`b` ベンチ) |
| `docs/theme-format.md` / `docs/app-guide.md` | テーマ形式 / アプリの追加方法 |
| `docs/theme-guide.md` | テーマ作成手順 (theme.json → ビルド → プレビュー → 実機) |
| `firmware/README.md` | 書き込み・ログ・シリアルのデバッグコマンド・操作方法 |
| `docs/archive/` | 終わった監査・提案・確認手順 (参照用。更新しない) |

## ルール
- `core/` は ESP-IDF / LVGL / FreeRTOS のヘッダを include しない。時刻・保存・ログは `core/include/watch/platform.hpp` のインターフェース経由。PC（Linux, g++/clang, CMake, GoogleTest）でビルド・テストできる状態を保つ。
- `firmware/` は `core/` を ESP-IDF コンポーネントとして取り込む（`firmware/components/watch_core/CMakeLists.txt` が `../../core` を参照）。
- LVGL を触るのは LVGL タスク内か `bsp_display_lock()` (= `lvgl_port_lock`) の中だけ。Feature は LVGL を知らない。
- Feature は静的リンク＋Registry。実行時プラグインは作らない。
- 実機で確かめていない値には `// TODO(hw): 実機で確認` を付ける。
- ユーザー向けの文字列とドキュメントは日本語。コードの識別子は英語。
- C++17、例外・RTTI は使わない（ESP-IDF 既定に合わせる）。動的確保は初期化時のみ。
- CI（GitHub Actions）が通らない変更は出さない。

## 実機で痛い目を見たので守ること
- **`firmware/managed_components/` を編集しない。** gitignore されていて CI や clean clone では消える。
  パッチが要るなら `firmware/vendor/` にコピーして `main/idf_component.yml` の `override_path` で差し替え、
  `firmware/vendor/README.md` の表に理由を書く。
- **LVGL タスクと SPI (QSPI 画面) の ISR は core1 に固定。** 別コアにすると spi_bus_lock の
  マルチコア競合で UI が凍る (docs/perf.md)。BLE は core0。
- **巨大な構造体をスタックに置かない** (ThemeManifest ~11.7KB で T23 のクラッシュループ)。
  `SAS_BIG_STATIC` (PSRAM) か `new (out) T()` で直接書く。
- **メモリ予算**: 内蔵 SRAM は常に逼迫している (BLE init に 32KB 連続、設定画面表示中は空き 1KB 未満)。
  大きいバッファは PSRAM へ。PSRAM も、画像テーマ + アプリ一覧では残り ~140KB しかない。
  常駐する大きな確保を足すときは、`b` ベンチの `psram_free` / `int_free` を全テーマで確かめる。
- **描画に効く変更** (LVGL 設定・フラッシュ経路・重い装飾・スナップショット) は、`b` ベンチの
  before/after を取って docs/perf.md に記録する。体感の主張だけで入れない。
- 実機書き込みはアプリだけ `0x20000` に書く (firmware/README.md)。ポート名は毎回 `ls /dev/ttyACM*` で確認。

## 検証コマンド
| 対象 | コマンド |
|---|---|
| core | `cmake -S core -B core/build && cmake --build core/build && ctest --test-dir core/build` |
| sim (デザイン確認) | `cmake -S sim -B sim/build && cmake --build sim/build -j && ./sim/build/snapshot --out sim/screenshots` (デザイン変更は前後のスクショを並べる。安全域・重なり・欠けグリフの監査は 0 件を保つ) |
| firmware | `cd firmware && . ~/esp/esp-idf/export.sh && IDF_CCACHE_ENABLE=1 idf.py build` |
| 実機の描画性能 | 書き込み後にモニタで `b` → `bench:` 行 (docs/perf.md) |
| android | `cd android && ./gradlew :protocol:test :agent:test :app:assembleDebug :app:lint` (JAVA_HOME = mise の temurin-17、SDK = `~/android-sdk`) |

## ドキュメントの更新
- タスクを終えたら、同じコミットで `docs/roadmap.md` の `状態:` と、`docs/handoff.md` の「現状」を更新する。
  実機で分かった値は handoff.md の表、ハードの事実は board.md に書く。
- 新しい調査結果を独立した文書にしたら、役目を終えたときに `docs/archive/` へ移す。
- 生成物 (ビルドディレクトリ、sim の出力、ログ) はコミットしない。スクショは `docs/screenshots*/` に置く。

## 同梱テーマ・フォント (assets パーティション)

テーマ zip は BLE 転送のほか、USB で assets パーティションへ直接書き込める。
`tools/mklfs_assets/make_assets_image.sh` が `android/app/src/main/assets/themes/*.zip`
を `/themes/<id>.zip` に並べた littlefs イメージ `firmware/build/assets_littlefs.bin`
を生成する (littlefs 設定は esp_littlefs と同一: block=4096, read/prog=128)。
`tools/fonts/bin/` があれば `*.bin` も `fonts/` に入る — これは T24 で
app バイナリから外した日本語フォント (jp_20/26/32) で、起動時に
`ui::init_display` が PSRAM へ読む。再生成は `python3 tools/gen_fonts.py --jp-bin`
(lv_font_conv 必須)。binfont が読めない時は内蔵の最小フォント
(font_jp_min_20、ASCII+かな) にフォールバックする。
書き込みは `python3 -m esptool write-flash 0x820000 firmware/build/assets_littlefs.bin`
(assets パーティションのみ。アプリ・NVS・storage には触れない)。
