# core/ — ハード非依存 C++17 Core

ESP-IDF / LVGL / FreeRTOS のヘッダを一切 include しない、PC でテストできる
Core と MVP Feature。設計は `docs/plan.md` C/D/E/G/L 章、通信仕様は
`docs/protocol-v1.md` に従う。

## 構成

```text
include/watch/          公開ヘッダ (interface + Feature の state アクセサ)
  platform.hpp          Clock / KeyValueStore / Log (実装は firmware 側)
  action.hpp            ActionType / Action (文字列は固定長バッファ)
  event.hpp, event_bus.hpp   EventType / 固定長購読表・同期 publish
  runtime.hpp           ActionQueue (ロックフック付き固定長リング) + Runtime
  navigation.hpp        Route / Navigator (最大4段 + モーダル)
  power.hpp             PowerPolicy (Active→Dim→ScreenOff→DeepSleepCandidate)
  input_mapper.hpp      ボタン → Action の割り当て表 (plan.md G章)
  feature.hpp           FeatureDescriptor / FeatureRegistry
  settings.hpp          protocol-v1.md の settings keys を型付きで保持
  protocol/             cbor.hpp / frame.hpp / dispatch.hpp
  features/             各 Feature の state アクセサ
src/                    Core 実装
features/               clock / timer / stopwatch / counter / memo
protocol/               crc16 / cbor / frame / dispatch 実装
tests/                  GoogleTest ホストテスト + フェイク (FakeClock 等)
sources.cmake           firmware/components/watch_core から参照する変数
```

## ビルド・テスト (PC)

```sh
cmake -S core -B core/build && cmake --build core/build
ctest --test-dir core/build
```

## firmware からの取り込み

`firmware/components/watch_core/CMakeLists.txt` から:

```cmake
include(${CMAKE_CURRENT_SOURCE_DIR}/../../../core/sources.cmake)
idf_component_register(SRCS ${WATCH_CORE_SOURCES}
                     INCLUDE_DIRS ${WATCH_CORE_INCLUDE_DIRS})
```

## ルール (AGENTS.md より)

- 例外・RTTI なし、動的確保は初期化時のみ。`-Wall -Wextra -Werror`
  `-fno-exceptions -fno-rtti` でビルドする (テスト側は GoogleTest のため例外あり)。
- 時刻・保存・ログは `platform.hpp` のインターフェース経由。
- 実機の無い前提なので、推測した値には `// TODO(hw): 実機で確認` を付ける。
