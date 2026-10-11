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
| **角R (パネル角丸)** | **≈100 px** | 実機写真の数値解析: 各辺フレームのクリップ量 85〜104px。対角方向の見え閾値 0.414·R≈41px で r=48(紫)リングのみ覗く観測と一致 |
| **パネル窓オフセット** | **x_gap=22, y_gap=0 は概ね正しい** | 4辺フレーム全てがアクティブ領域端ギリギリに表示。左上↔右下の見え方差は 2.5D 湾曲ガラスの視差で、窓ずれではない |
| **safe-area inset** | **32〜36px** | 矩形セーフ域: 隅点が見える条件 s ≥ R(1−1/√2) ≈ 29px → 余裕込みで 36。`kPad`(content 左右余白) を 22→32、mascot 角クランプを 36 に反映済み |
| **待機 redraw** | **0 (コード側の無駄再描画なし)** | LOG_MODE 計測: 無操作時 `redraw_cnt: 0`、refr timer は常時 30Hz で dirty チェックのみ |
| **描画コスト** | **render ~50–190ms/全面** | 起動直後の sysmon ログ。全面 redraw 1回が重い → 全面更新の実効 FPS 上限 ~5–6 |
| **「チラチラ」の原因** | **perf overlay の自己再描画 (確定)** | overlay は 300ms 固定周期でラベル更新→部分 flush。LOG_MODE (overlay 非表示) ビルドで揺れ消失を実機確認 |
| perf monitor CPU% | **信用できない** | tickless idle + light sleep 下で idle 計測が破綻し 100% 表示になる |

要実機体感 (ユーザー判断):

| 項目 | 試し方 |
|---|---|
| deep sleep→PWR 体感 | `settings.set {deep_sleep_after_s: N}` で値を変えて試す (即時反映・再起動不要)。既定 1800s |

## safe-area 反映済み箇所

- `components.cpp` `kPad` 22→32 (content() 列の左右余白)
- `components.cpp` mascot 角クランプ 22→36、ヘッダ小型 mascot も角を避けて 6px 下げ
- 残り: faces の文字盤内要素・alert ボタン幅 366 など、デザイン意匠が絡む所は Opus 判断

値の取り方 (再掲): 四隅の欠け始め半径 R → 隅からの斜めオフセットは `R·(1−1/√2)≈0.293·R`。

## Opus に残るデザイン系タスク

- **フォント不足**: 日本語フォントは `font_jp_20` / `font_jp_26` のみ。サイズ展開と
  ウェイト (細い報告あり) の検討・追加は Opus の領域
- **safe-area padding の最終値**: 上記の機械的 inset が入ったあと、各画面の
  余白バランスはデザイン判断
- **色・レイアウトの統一感**: テーマ cosmos/cyber/cute の整い方全般
- パーツギャラリー画面 (screenshot の 77 相当) で各ウィジェットの見え方を確認

## 未解決の技術課題

### 1. ~~「チラチラ」~~ → 解決済み

原因は **perf overlay の 300ms 自己再描画** (LOG_MODE ビルドで消失を実機確認)。
コード側に待機中の無駄更新は存在しない (redraw_cnt=0)。
`sdkconfig.defaults` で `LV_USE_PERF_MONITOR_LOG_MODE=y` を既定化済み
(overlay 非表示・UART に `sysmon:` 出力)。

### 2. 描画の重さ — 残課題 (render 50–190ms/全面)

実測: 全面 redraw 1回 ~50–190ms → 実効 FPS ~5–6 が上限。スクロール中の
「紙芝居」感の本体。候補 (未検証):

- LVGL オブジェクトは `SPIRAM_MALLOC_ALWAYSINTERNAL=1024` で PSRAM 行き
  → スタイル/レイアウト走査が octal PSRAM にヒットして遅い可能性
- refr timer 30Hz での dirty チェック自体は軽い (redraw=0 でも 30Hz で回る)
- QSPI 転送: height10 で 50 チャンク/全面 (double_buffer で重畳は効く)

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
