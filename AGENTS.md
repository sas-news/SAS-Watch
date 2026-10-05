# AGENTS.md

作業前に `docs/plan.md`（設計）、`docs/board.md`（ハード）、`docs/protocol-v1.md`（BLE仕様）を読むこと。

## ルール
- `core/` は ESP-IDF / LVGL / FreeRTOS のヘッダを include しない。時刻・保存・ログは `core/include/watch/platform.hpp` のインターフェース経由。PC（Linux, g++/clang, CMake, GoogleTest）でビルド・テストできる状態を保つ。
- `firmware/` は `core/` を ESP-IDF コンポーネントとして取り込む（`firmware/components/watch_core/CMakeLists.txt` が `../../core` を参照）。
- LVGL を触るのは LVGL タスク内か `bsp_display_lock()` の中だけ。Feature は LVGL を知らない。
- Feature は静的リンク＋Registry。実行時プラグインは作らない。
- 実機が無い前提で書いているので、推測した値には `// TODO(hw): 実機で確認` を付ける。
- ユーザー向けの文字列とドキュメントは日本語。コードの識別子は英語。
- C++17、例外・RTTI は使わない（ESP-IDF 既定に合わせる）。動的確保は初期化時のみ。
- CI（GitHub Actions）が通らない変更は出さない。
