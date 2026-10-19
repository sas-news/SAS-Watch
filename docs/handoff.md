# handoff.md — 現状 (2026-10-19)

次に作業する人が最初に読む「今どうなっているか」。残タスクと方針は `docs/roadmap.md`。
過去の経緯 (2026-10-15 までの引き継ぎ文書、T14 監査など) は `docs/archive/handoff-2026-10-15.md`。

## 要点

- 対象個体: Waveshare ESP32-S3-Touch-AMOLED-2.06 (32MB flash / PSRAM 8MB)。スピーカーは無効、
  マイクは I2S DMA の内蔵 RAM 不足で録音できない (T18 保留)。
- 同梱テーマ 4 つ (mame/cosmos/cute/cyber) は assets パーティションに USB で書いてある (AGENTS.md)。
  BLE でのテーマ転送は不安定で先送り (T25)。
  **注意**: 実機の assets に入っている cute.zip は、リポジトリの `android/.../themes/cute.zip`
  より古い (bg.png が b6dd640 以前のもの)。必要なら assets イメージを作り直して書く。
- 2026-10-19 に描画を作り直した (T29)。standard テーマで全面再描画 85〜112ms → 40〜68ms。
  経緯と数値は `docs/perf.md`。
- 外部コンポーネントへのパッチは `firmware/vendor/` にある (BSP / esp_lvgl_port / sh8601)。
  `managed_components/` は編集しない。

## 実機で確定した値

| 項目 | 値 | 根拠 |
|---|---|---|
| 描画パイプライン | PSRAM の全面描画バッファ 1 枚 + 内蔵 8.2KB バウンス 2 枚 | docs/perf.md |
| LVGL タスク / SPI ISR | 両方 core1 固定 | spi_bus_lock のマルチコア競合 (IDFGH-17166) で UI が凍った |
| 全面再描画 (standard) | 40〜68ms (`b` ベンチ) | docs/perf.md |
| 全面再描画 (cute/cyber) | 75〜225ms。スキンの重い画面 (設定・タイマー) が遅い | docs/perf.md |
| 内蔵 SRAM | 起動後の空き ~21KB。BLE init に 32KB 連続が要る。設定画面表示中は空き数百バイト | `b` の int_free |
| PSRAM | standard で空き ~2.9MB、画像テーマのアプリ一覧で ~140KB (mame) | `b` の psram_free |
| app 領域 | 4MB 中の空き 8% | T24 (日本語フォントを外に出す) で戻す予定 |
| VSYS power-off 閾値 | 2600 mV (既定維持) | AXP2101 reg 0x24 |
| 角 R / safe-area | R≈100px / 32〜36px | 実機写真の解析 |
| パネル窓オフセット | x_gap=22, y_gap=0 | 同上 |
| 待機中の再描画 | 0 (無操作で redraw なし) | LVGL sysmon |
| `deep_sleep_after_s` | BLE `settings.set` で即時反映 | core/protocol/dispatch.cpp |
| touch の I2C NACK | ~14 秒に 1 回、実害なし | FT3168 の省電力周期と推定 |
| 起床ゲート (腕上げ) | `静止3連続 && deg>=30 && dir.z<=-0.65 && dir.x>=-0.35` (左手首で計測) | roadmap T27 |

## 未解決・注意

- 録音できない (T18)。描画バッファの見直しで内蔵 SRAM が起動時に 8KB 増えたので、再挑戦の余地がある。
- app 領域の空きが 8% (T24)。
- BLE のテーマ転送 (T25)。
- 時計の設定画面からファイルテーマを選べない (T28)。
- Home と文字盤の整理はユーザーの判断待ち (T22、`docs/home-face-proposal.md`)。
- 起動時に `E sh8601: swap_xy is not supported by this panel` が 1 行出るが無害
  (esp_lvgl_port が sw_rotate=false のとき swap_xy を呼ぶ。パネル側が未実装なだけ)。
- 画像テーマ (cosmos/cute/cyber) で、アプリ一覧の bake (~850KB) が PSRAM の断片化で
  確保できないことがある。そのときは bake せずに普通の一覧で表示される (見た目は同じで少し重い)。

## 実機作業の注意

- アプリの書き込みは `0x20000` (ota_0)。ほかのオフセットはパーティションを壊す。手順は firmware/README.md。
- ポートは `/dev/ttyACM0` ↔ `/dev/ttyACM1` で変わる。毎回 `ls /dev/ttyACM*`。
- `idf.py flash` は cmake を再実行する。`IDF_CCACHE_ENABLE=1` 無しだと全リビルド。
- ログは `python -m esp_idf_monitor --port /dev/ttyACM0 build/sas_watch.elf` を `timeout` 付きで
  ファイルに落とすのが安定。自動操作は pyserial で 1 文字コマンド (`b` `t` など) を送る。
- 画面 OFF 中の入力 (シリアルの `t` もタッチ扱い) は「起こすだけ」で処理されない。
- Android 13 以降のサイドロードでは「制限された設定」を許可しないと通知アクセスを ON にできない
  (設定 → アプリ → SAS-Watch → ⋮ → 制限された設定を許可)。

## 検証済み環境

ESP-IDF 5.5.1 / LVGL 9.5.0 / esp_lvgl_port 2.9.0 (vendor) / NimBLE / C++17 (例外・RTTI なし)。
