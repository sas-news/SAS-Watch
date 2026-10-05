# amoled-watch

Waveshare **ESP32-S3-Touch-AMOLED-2.06** 向けの自作スマートウォッチ。

- `firmware/` … ESP-IDF 5.5 + 公式BSP (`waveshare/esp32_s3_touch_amoled_2_06`) + LVGL 9
- `core/` … ハードに依存しないC++17のCore/Feature（PCでテストできる）
- `android/` … Android 14 向けコンパニオンアプリ (Kotlin / Jetpack Compose / BLE)
- `docs/` … 設計 (`plan.md`)、ハード情報 (`board.md`)、通信仕様 (`protocol-v1.md`)

> 実機が届く前に公式サンプル・回路図から推測で書いている部分がある。`docs/board.md` の【要実機】を実機で確認して直す。

## ビルド

```sh
# Core のテスト (PC)
cmake -S core -B core/build && cmake --build core/build && ctest --test-dir core/build

# Firmware (ESP-IDF 5.5)
cd firmware && idf.py set-target esp32s3 && idf.py build

# Android
cd android && ./gradlew assembleDebug
```
