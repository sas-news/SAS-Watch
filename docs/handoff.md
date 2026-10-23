# handoff.md — 現状 (2026-10-22)

次に作業する人が最初に読む「今どうなっているか」。残タスクと方針は `docs/roadmap.md`。
過去の経緯 (2026-10-15 までの引き継ぎ文書、T14 監査など) は `docs/archive/handoff-2026-10-15.md`。

## 要点

- 対象個体: Waveshare ESP32-S3-Touch-AMOLED-2.06 (32MB flash / PSRAM 8MB)。スピーカーは無効、
  マイクは録音できる (T18 解決: I2S RX/DMA を `audio::init` で予約し、
  `esp_codec_dev_read` の戻り値解釈を修正。実機で 5.5s/88K サンプル確認。
  2026-10-22 にメモ画面からの録音・保存もユーザーが確認)。
  音声メモのスマホ転送はエラーで、音声メモをほぼ使わないため先送り (T30)。
- 設定画面のテーマ節に assets の zip テーマが出る (T28)。sim では行タップで
  `settings.theme` が変わりテーマが適用されることを確認。実機は書き込み・起動・
  設定画面生成 (bench 90ms) まで確認済みで、タップと再起動持続だけ未確認。
- 歩数計: QMI8658C の HW ペドメータを修正 (CTRL9 の ACK 漏れと INT1
  ハンドシェイク、パラメータの毎ブート再投入、STATUS1.bit4=Pedometer を
  起床と誤判定していた件)。2026-10-23 に実機で歩数が増えることをユーザーが確認。
- 音声メモの文字起こし (T31): 時計の「文字起こし」→ 電話の STT →
  `memo.text.set` で音声メモに本文を保存。T30 の BULK 修正 (2026-10-23)
  後に実機再確認する。
- 同梱テーマ 4 つ (mame/cosmos/cute/cyber) と日本語フォント 3 つ (jp_20/26/32 の
  binfont、計 ~1.4MB) は assets パーティションに USB で書いてある (AGENTS.md)。
  フォントは起動時に `ui::init_display` で PSRAM へ読み、失敗時は内蔵の最小
  フォント (ASCII+かな+記号) にフォールバックする (T24)。
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
| 全面再描画 (cute/cyber) | 80〜157ms (設定は最大 ~160ms)。スキンの大きい部品は近似塗りに置き換え済み | docs/perf.md |
| 内蔵 SRAM | 起動後の空き ~21KB、表示中 ~18-21KB (LVGL ヒープを PSRAM に移して安定化) | `b` の int_free |
| PSRAM | standard で空き ~3.4MB (アリーナ不要)。肌テーマで ~750KB、 mame で ~450KB 空き (binfont 常駐 ~1.4MB + アリーナ) | `b` の psram_free |
| app 領域 | 4MB 中の空き ~31% (日本語フォントを assets の binfont へ移した T24) | `idf.py build` 末尾 |
| VSYS power-off 閾値 | 2600 mV (既定維持) | AXP2101 reg 0x24 |
| 角 R / safe-area | R≈100px / 32〜36px | 実機写真の解析 |
| パネル窓オフセット | x_gap=22, y_gap=0 | 同上 |
| 待機中の再描画 | 0 (無操作で redraw なし) | LVGL sysmon |
| `deep_sleep_after_s` | BLE `settings.set` で即時反映 | core/protocol/dispatch.cpp |
| touch の I2C NACK | 解消。FT3168 スリープ中のセーフティポーリングを INT (GPIO38) ゲートで止めた | vendor/esp_lvgl_port |
| 起床ゲート (腕上げ) | `静止3連続 && deg>=30 && dir.z<=-0.65 && dir.x>=-0.35` (左手首で計測) | roadmap T27 |

## 未解決・注意

- BLE のテーマ転送 (T25)。
- 音声メモをスマホへ送るとエラーになる (T30)。原因は BULK 送信の mbuf 枯渇で、
  2026-10-23 に修正・書き込み。実機再確認待ち。
- 設定画面のファイルテーマ選択 (T28) は実装済み。実機はタップと再起動持続が未確認。
- 歩数計 (HW ペドメータ) の修正は実機で歩数増加を確認済み (2026-10-23)。
- 重いテーマ (cute/cyber) で固まることがある (ユーザー報告)。テーマ巡回 +
  bench 5 周では再現しなかった。再現手順 (テーマ・操作・画面) を採取中。
- Home と文字盤の整理はユーザーの判断待ち (T22、`docs/home-face-proposal.md`)。
- 起動時の `E sh8601: swap_xy is not supported by this panel` は vendor の
  esp_lvgl_port をパッチして消した (回転なしボードでは呼ぶ意味がない)。
- 肌テーマ (cute/cyber) ではアプリ一覧の bake (~850KB) がほぼ確実に失敗する
  (T24 の binfont 常駐 ~1.4MB + アリーナで、最大連続空きは ~500KB)。
  見た目は同じだがスクロールが ~180ms と遅い。bake 確保失敗時はスキン
  キャッシュを捨てて一度だけ再試行する。テーマアリーナは「資産領域」と
  「画面ブロック 640KiB×2」に分割して別個確保する — フォント常駐後は
  単一 ~3MB の連続確保が失敗して mame の資産が欠けていたため。
- スキン (9-slice) の大きい部品は 1D 走査で近似塗りに切り替えた (fill/band、詳細は
  docs/perf.md)。cute は sim 差分 ~0、cyber は縞の補間差のみ (max<=17) で性能が
  およそ 1.3〜1.8 倍になった。縁ストリップは LVGL の伸縮補間が見た目の一部なので
  近似していない (やると cute で max=146 の誤差が出る)。

## 実機作業の注意

- アプリの書き込みは `0x20000` (ota_0)。ほかのオフセットはパーティションを壊す。手順は firmware/README.md。
- ポートは `/dev/ttyACM0` ↔ `/dev/ttyACM1` で変わる。毎回 `ls /dev/ttyACM*`。
- `idf.py flash` は cmake を再実行する。`IDF_CCACHE_ENABLE=1` 無しだと全リビルド。
- ログは `python -m esp_idf_monitor --port /dev/ttyACM0 build/sas_watch.elf` を `timeout` 付きで
  ファイルに落とすのが安定。自動操作は pyserial で 1 文字コマンド (`b` `t` `v` など) を送る。
  素の `cat` は制御線の扱いで受信できないことがある — 1 セッションで open→送信→受信まで行う
  (DTR/RTS を自分でいじるとチップが download モードでリセットループするので触らない)。
- 画面 OFF 中の入力 (シリアルの `t` もタッチ扱い) は「起こすだけ」で処理されない。
- Android 13 以降のサイドロードでは「制限された設定」を許可しないと通知アクセスを ON にできない
  (設定 → アプリ → SAS-Watch → ⋮ → 制限された設定を許可)。

## 検証済み環境

ESP-IDF 5.5.1 / LVGL 9.5.0 / esp_lvgl_port 2.9.0 (vendor) / NimBLE / C++17 (例外・RTTI なし)。
