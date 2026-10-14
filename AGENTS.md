# AGENTS.md

作業前に `docs/plan.md`（設計）、`docs/board.md`（ハード）、`docs/protocol-v1.md`（BLE仕様）を読むこと。
残タスクと決定済みの方針は `docs/roadmap.md`。そこにある順・方針どおりに進め、各タスクの `状態:` を更新する。

## ルール
- `core/` は ESP-IDF / LVGL / FreeRTOS のヘッダを include しない。時刻・保存・ログは `core/include/watch/platform.hpp` のインターフェース経由。PC（Linux, g++/clang, CMake, GoogleTest）でビルド・テストできる状態を保つ。
- `firmware/` は `core/` を ESP-IDF コンポーネントとして取り込む（`firmware/components/watch_core/CMakeLists.txt` が `../../core` を参照）。
- LVGL を触るのは LVGL タスク内か `bsp_display_lock()` の中だけ。Feature は LVGL を知らない。
- Feature は静的リンク＋Registry。実行時プラグインは作らない。
- 実機が無い前提で書いているので、推測した値には `// TODO(hw): 実機で確認` を付ける。
- ユーザー向けの文字列とドキュメントは日本語。コードの識別子は英語。
- C++17、例外・RTTI は使わない（ESP-IDF 既定に合わせる）。動的確保は初期化時のみ。
- CI（GitHub Actions）が通らない変更は出さない。

## 同梱テーマ (assets パーティション)

テーマ zip は BLE 転送のほか、USB で assets パーティションへ直接書き込める。
`tools/mklfs_assets/make_assets_image.sh` が `android/app/src/main/assets/themes/*.zip`
を `/themes/<id>.zip` に並べた littlefs イメージ `firmware/build/assets_littlefs.bin`
を生成する (littlefs 設定は esp_littlefs と同一: block=4096, read/prog=128)。
書き込みは `python3 -m esptool write-flash 0x820000 firmware/build/assets_littlefs.bin`
(assets パーティションのみ。アプリ・NVS・storage には触れない)。
