# SAS-Watch

Waveshare **ESP32-S3-Touch-AMOLED-2.06** 向けの自作スマートウォッチ。
実機 (32MB flash / PSRAM 8MB) で日常的に動かしながら開発している。

| ディレクトリ | 中身 |
|---|---|
| `firmware/` | ESP-IDF 5.5 + LVGL 9.5。書き込み・ログ・デバッグコマンドは `firmware/README.md` |
| `core/` | ハードに依存しない C++17 の Core/Feature (PC でテストできる) |
| `apps/` | アプリ (`apps/<id>/app.cmake` を置くと自動で登録される。`docs/app-guide.md`) |
| `sim/` | PC 上で LVGL 画面を描いて PNG を出すシミュレータ (デザイン確認と監査) |
| `themes/` / `tools/` | テーマの素材とビルドスクリプト、テーマスタジオ、フォント生成など |
| `android/` | Android 14 向けコンパニオンアプリ (Kotlin / Jetpack Compose / BLE) |
| `docs/` | 下の表 |

## ドキュメント

| ファイル | 中身 |
|---|---|
| `docs/handoff.md` | 現状 (実機で確定した値・未解決の問題) |
| `docs/roadmap.md` | 残タスクと方針 |
| `docs/plan.md` | 設計 |
| `docs/board.md` | ハード情報 |
| `docs/protocol-v1.md` | BLE 通信仕様 |
| `docs/perf.md` | 描画パイプラインと性能計測 |
| `docs/theme-format.md` | テーマパッケージの形式 |
| `docs/app-guide.md` | アプリの作り方 |
| `docs/walk-event.md` | 歩く大会モード (ウォーク) の設計・使い方・電池 |
| `docs/bringup.md` | 新しい実機を立ち上げる手順 |
| `docs/archive/` | 終わった監査・提案・確認手順 |

開発のルールは `AGENTS.md`。

## ビルド

```sh
# Core のテスト (PC)
cmake -S core -B core/build && cmake --build core/build && ctest --test-dir core/build

# シミュレータ (全画面の PNG を sim/screenshots に出す)
cmake -S sim -B sim/build && cmake --build sim/build -j && ./sim/build/snapshot --out sim/screenshots

# Firmware (ESP-IDF 5.5)
cd firmware && idf.py set-target esp32s3 && idf.py build

# Android
cd android && ./gradlew assembleDebug
```
