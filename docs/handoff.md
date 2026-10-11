# 引き継ぎ文書 (Opus 向け)

機械系タスクの確定値と、残りのデザイン系タスク・未解決技術課題のまとめ。
対象個体: Waveshare ESP32-S3-Touch-AMOLED-2.06 (スピーカー/コーデック非搭載個体、
`CONFIG_WATCH_DISABLE_AUDIO=y`)。

## 実機で確定した値

| 項目 | 値 | 根拠 |
|---|---|---|
| VSYS power-off 閾値 (AXP2101 reg 0x24) | **2600 mV** | 起動ログ `vsys power-off threshold=`。リチウム実用下限として妥当 → **既定維持で判断済み** |
| LVGL バッファ | 410×10行×2枚 (double_buffer) | height20×2枚 (32.8KB) は内蔵SRAMを圧迫し BLE_INIT の 32KB 連続確保が失敗してリブートループ。10行×2=16.4KB で共存 (PR #34) |
| 内蔵SRAM 余裕 (audio無効時) | 数KB 程度 | I2S init の NO_MEM / BLE malloc 失敗の履歴から、常に逼迫していると思ってよい |
| `deep_sleep_after_s` の反映 | **BLE settings.set で即時反映** | `h_settings_set` → `apply_settings_side_effects` → `PowerPolicy::configure` (core/protocol/dispatch.cpp:672)。再起動不要 |

要実機計測 (ユーザーの build/flash 確認待ち):

| 項目 | 計測方法 |
|---|---|
| 角R (切れ始める半径) | `WATCH_DIAG_CORNERS=y` の四隅リング: 赤=0 → 紫=48px、8px 刻み。**見えなくなり始めるリング色を報告してもらう** |
| 待機 FPS / CPU% | 画面上端中央の perf monitor。無操作時の数値を記録 |
| deep sleep→PWR 体感 | `settings.set {deep_sleep_after_s: N}` で値を変えて試す (即時反映・再起動不要)。既定 1800s |

## 角R → safe-area 反映先 (値が決まったら)

テーマ manifest の `pad` は skin 飾り用で既に加算機構あり
(`components.cpp` `apply_skin`, use_pad 経路)。ハード固有の safe-area は
テーマに書かず、**コード側の固定 inset** にするのが筋:

- `faces/faces.cpp` — 文字盤ルートの生成箇所 (現状 pad_all=0)
- `components.cpp` — 画面 content 領域
- 候補: `ui::` 名前空間に `kSafeInsetPx` のような定数を置き、上記2箇所で使う

値の取り方: 四隅の欠け始め半径 R → 隅からの斜めオフセットは `R·(1−1/√2)≈0.293·R`。
実効 inset はおおよそ `R×0.3` + 見た目の余裕分。例: R=48 なら inset ≈14〜18px。

## Opus に残るデザイン系タスク

- **フォント不足**: 日本語フォントは `font_jp_20` / `font_jp_26` のみ。サイズ展開と
  ウェイト (細い報告あり) の検討・追加は Opus の領域
- **safe-area padding の最終値**: 上記の機械的 inset が入ったあと、各画面の
  余白バランスはデザイン判断
- **色・レイアウトの統一感**: テーマ cosmos/cyber/cute の整い方全般
- パーツギャラリー画面 (screenshot の 77 相当) で各ウィジェットの見え方を確認

## 未解決の技術課題

### 1. 「チラチラ」(無操作で色が揺れる) — 最有力: perf monitor 自身

コード側の定期 invalidate は以下のみで、待機中は事実上ゼロ:

- 文字盤: 分単位 (`face_minimal.cpp:134`, `face_analog.cpp:125`)
- stopwatch 50ms / timer 250ms: その画面を開いている間だけ

一方 `LV_USE_PERF_MONITOR` のオーバーレイは **300ms 固定周期**
(`lv_sysmon.c` の `LV_SYSMON_REFR_PERIOD_DEF`, kconfig 不可) で自身のラベルを
更新し、その都度部分 invalidate → flush が走る。待機中の揺れはほぼこれと見る。

切り分け:

- perf monitor を `n` (sdkconfig) にして揺れが消えれば確定
- FPS/CPU% の値だけ欲しい場合は `CONFIG_LV_USE_PERF_MONITOR_LOG_MODE=y`
  (overlay なし・UART ログ出力) に切り替える手がある
- 残るなら表示側: 部分フラッシュの見え方 / 輝度遷移 / AMOLED パネル固有

### 2. 描画の重さ — 要実機数値

perf monitor の FPS/CPU% で切り分け:

- CPU% 高い → 描画負荷 (LVGL オブジェクトは
  `SPIRAM_MALLOC_ALWAYSINTERNAL=1024` で PSRAM 行き → PSRAM アクセス遅延が
  描画に効く可能性。CPU 周波数も要確認)
- CPU% 低いのに重い → QSPI 転送帯域 (height10 化でチャンク数 50/全面更新に増)

### 3. `action queue full; dropped 40` (一過性)

タッチ indev TIMER 化後の1ブートで13件出たが、次ブートでは0件・電源遷移正常。
タッチ連打等の実入力による輻輳なら問題ないが、無操作で再発するなら上流案件。

### 4. その他留意

- deep sleep → PWR 復帰はフルリブート (本質的に遅い)。入る頻度を減らすなら
  `deep_sleep_after_s` を上げる (即時反映済み) のが確実な手。PM lock で
  light sleep 抑止は wake 遅延への効果が薄い (light sleep 復帰は元々速い)
- 音声関連: この個体は `CONFIG_WATCH_DISABLE_AUDIO=y` で全スキップ。
  littlefs `/storage` マウントは audio init 内で行われるが無効化時も残る
- `ble_link_set_advertising` (PR #35): 画面OFF中は広告停止・接続維持の C案

## 検証済み環境

- ESP-IDF 5.5.1 / LVGL 9.5.0 / NimBLE / C++17 (例外・RTTI なし)
- `IDF_CCACHE_ENABLE=1` 推奨 (フルリビルド ~7分 → ~55秒)
- core ホストテスト: `cd core/build && ctest` (189件)
