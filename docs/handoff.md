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
「紙芝居」感の本体。内訳は render (CPU側ラスタライズ) が支配的で
待機・単発再描画時の flush (QSPI転送) は 5–15ms に過ぎない。

**スクロール/ドラッグ中はさらに悪い** (fprobe 実測): render 280–365ms +
flush 225–335ms で 1フレーム ~400–500ms → 実効 ~2fps。flush 爆発の構造:
スクロールは LVGL の無効領域を content 幅 (346px) ×バッファ高10行の帯に
分割して flush するため 1フレーム ~50回の個別 draw_bitmap が走り、各回
CASET/RASET (ポーリング送信・先行DMA全件待ち) + RAMWR キューを逐次処理。
送信サブミット自体は 0–15ms/回だが、転送と描画が交互にブロックし合い
合計で重くなる。**config レベルの加速手段は実機で全て棄却済み**:

- **PPA**: ESP32-S3 にはハード自体が無い (P4系のみ)。LVGL Kconfig の
  `LV_USE_PPA` は有効化しても `#error "This SoC does not support PPA"` で
  ビルド不可
- **S3 SIMD asm** (esp_lvgl_port 2.9.0 内蔵): CMake が LVGL 9.1.x のみに
  ゲートしており 9.5 では非対象。blend API が 9.3 で変わったためそのまま
  では動かず、移植は上流規模の作業 (esp-bsp issue #621 が該当)
- **`LV_DRAW_SW_DRAW_UNIT_CNT=2`** (デュアルコア並列描画): 実機で悪化。
  起動描画 render 285–315ms (CNT=1 では 125–220ms) + drawスレッド分の
  8KBスタック増 + `lv_os_get_idle_percent` 警告の毎秒化
- **`LV_DRAW_BUF_STRIDE_ALIGN=64`**: 効果なし (render ~120ms で不変)

残る本筋のレバー:

- **デザイン側の軽量化** (Opus 領域): shadow/glow・gradient・AAテキストの
  抑制。`components.cpp` の glow (shadow_width/opa=120) や face 側の
  効果描画が画素単位コストを積んでいる
- **上流の S3 SIMD 移植** or `espressif/lvgl` フォーク評価 (LVGL 9.x で
  PIE命令最適化が取り込まれたら差し替え候補)
- 参考: LVGL オブジェクトは `SPIRAM_MALLOC_ALWAYSINTERNAL=1024` により
  ~200B/個は内蔵SRAM、フォント等の大物は PSRAM。QSPI 転送は height10で
  50チャンク/全面だが double_buffer 重畳で既に速い

### 3. ~~`action queue full; dropped 40`~~ → 根本原因を特定・修正済み

**原因**: `on_touch_press` (esp_display.cpp) が indev の `LV_EVENT_PRESSED`
毎に `Wake` Action を emit していたが、このイベントは押下中 **read 毎
(~66回/秒)** に発火する。指を押さえ続けるだけで ActionQueue (40件) が
Wake で溢れ、同時期に来た Back/PrimaryAction 等の本物の入力が drop
されていた = **「戻るボタン等の反応が悪い」の主犯**。

**修正**: `on_touch_press` 内で Wake emit を 400ms に1回に間引き
(`s_last_wake_emit`)。消灯中の最初のタップは必ず間隔が空いているので
即時 emit される＝wake-on-touch は維持。

**副次的知見**: 長い render 中 (~300–500ms) は LVGL タスクが占有され
indev read_timer が飢餓 → タッチサンプルに 300ms 級のギャップが生じ
座標が飛ぶ (tprobe 実測)。ドラッグ中の「座標ジャンプ」はこれ。

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

## 実機操作での失敗と教訓 (再発防止)

- **アプリのフラッシュオフセットは `0x20000` (ota_0)**。`0x110000` に
  書くとパーティション途中を破壊 → `No bootable app partitions` で
  ブートループし、連続リセット中は USB-Serial が列挙できず
  `/dev/ttyACM0` 自体が消える (実際に踏んだ)。リカバリ: ケーブル
  抜き差し or 電源入れ直しでポート復活後、`0x20000` に書き直す
- アプリのみの再フラッシュは esptool 直叩きが速い
  (~25秒・再cmake 無し):
  `python components/esptool_py/esptool/esptool.py -p /dev/ttyACM0
  -b 460800 --before default_reset --after hard_reset --chip esp32s3
  write_flash 0x20000 build/sas_watch.bin`
- **`idf.py -p flash` は cmake を再実行する**: `IDF_CCACHE_ENABLE=1`
  の env 無しで呼ぶと `CCACHE_ENABLE=0` で 2145 ターゲットの
  全リビルドに化けた (実際に踏んだ)。フラッシュ時も env を渡すか、
  上記 esptool を使う
- 監視は `python -m esp_idf_monitor --port /dev/ttyACM0 build/sas_watch.elf`
  を `timeout` 付きでファイルにリダイレクトするのが安定
