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

## Opus 5.5 への統合タスクリスト

機械系のボトルネックは大方解消したので、UX/デザイン/機能仕上げを任せる。
優先度はユーザー体感順。「実機確認必須」のものは Devin 側と連携。

### A. 操作感・UX (時計側)

- **全体的な滑らかさ**: render 50–190ms/全面 の構造コストが残る
  (§2)。スクロール追随・画面遷移・秒針/カウンタ更新の体感を磨く。
  重い画面の特定→軽量化はデザイン判断とセットで
- **消灯→タップ復帰時の誤タップ**: 起こしタップの座標がそのまま
  画面上のボタンに当たって誤発火する。復帰タップは Wake だけにして
  UI には流さない仕組みが要る (runtime.cpp の dispatch で ScreenOff
  中の外部入力は「起こすだけ」にしているが、押下継続中の
  PRESSED→RELEASE シーケンスが LVGL 側に残り、復帰後の要素を
  誤タップする可能性。要検証)
- **ボタン操作の即応性**: Boot ボタンがダブルクリック判定待ちで
  ~300–500ms 遅延する。単押しを即時化 (ダブルクリックを別ボタン/
  別操作に逃がす or 遅延を縮める)。ストップウォッチの Start/Stop も
  同じ遅延パスを通るなら即応性を確認
- **USB 接続中はスリープしない設定**: PC 接続中に画面OFF→deep sleep
  されるとデバッグしにくい。VBUS 検知 (AXP2101 は isVbusIn 取得可)
  で deep sleep / screen off を抑制する設定項目
- ~~**腕上げ点灯 (raise-to-wake) / 歩数 / IMU 系が正しく動いていない**~~
  → 原因特定・修正済み (実機での腕上げ確認は未):
  - vendored `qmi8658_enable_wake_on_motion` は WoM 閾値を書くだけで
    CTRL9 `CTRL_CMD_WRITE_WOM_SETTING` (0x08) を送らず **WoM が一度も
    有効になっていなかった**。`board::imu::arm_wake_on_motion` を
    QMI8658C DS §9.4 Fig.13 準拠で実装 (CAL1_L=閾値mg, CAL1_H[7:6]=
    INT選択・初期値, CAL1_H[5:0]=blanking → GPIO21=INT1 init0+blanking4)
  - light sleep は未使用 (esp_pm 無効) ので `esp_sleep_get_wakeup_cause`
    を見る従来経路は絶対に発火しない → GPIO21 に本物の GPIO ISR
    (POSEDGE、WoM INT はイベント毎トグルで STATUS1 読取で初期値戻り)
    → imu_task 通知 → burst → RaiseDetector。常設ログ `wom int` /
    `raise detected -> wake` あり
  - hw pedometer は WoM 中も STEP_CNT 読取可 (arm 前後で ESP_OK、
    連続性の実機検証は未)
  - touch の `FT5x06 I2C read error` は ~13.8s に1回の単発 NACK (reader
    タスク 100ms ポーリングで即復帰、wake-on-touch への影響なし) — 
    FT3168 の省電力周期と推定、実害なしでコード変更なし
- **直感的でない操作の修正**: ユーザー体感としての違和ポイント全般
  (戻る/確定の一貫性、スワイプ方向など)

### B. 時刻のバグ (時計側)

- **時(hour)が0から始まる**: 分は正しいのに時だけ 0 表示/0 から
  カウント。`time.set` 適用後もしばらくすると戻る。RTC (PCF85063)
  への書き戻しと、TZ (JST-9) / epoch 変換のどこかで hour が潰れて
  いる疑い。`EspClock::set_epoch_s` → `board::rtc::set_epoch` の
  変換を確認。UTC epoch を localtime で表示する際の TZ 二重適用、
  または RTC 読み出し側の BCD 変換ミスが候補

### C. メディア (時計↔電話)

- **アルバムアート表示**: `media.state` に画像を載せる経路を追加。
  小さいので BULK or 新フレーム種で JPEG/PNG (or 縮小 RGB) を送り、
  media 画面に表示。Android 側は MediaMetadata の ART/ALBUM_ART を
  縮小して送る
- ~~**`internal write failed` の定期発生**~~ → 確定・修正済み:
  「NOTIFY_TX ~300-700ms」の正体は接続 handshake (7 req) と、
  それに続く **`media.state` の ~1s/混雑時 ~360ms 連発** (一時
  `req op=` ログで実測)。時計側の EVT spam (att_handle=19) は
  0件だった。電話側は `writeRaw` が false を返すと
  `Res.Err("internal","write failed")` — GATT 操作の重なりで
  writeCharacteristic が失敗する。
  - 修正 (Android): `BleWatchConnection` の GATT 操作を opMutex
    で完全直列化 (connectOnce の discover/mtu/CCCD も含む) +
    REQ 単位の reqMutex (複数フレームのインターリーブ防止)。
    `MediaBridge` に `MediaStateCoalescer` (同一抑制 + 500ms
    間引き + trailing) を追加。一時ログ (req op / evt notify /
    steps dbg) は撤去済み
  - 原因として残る可能性: media.state が他の write と同期中に
    割り込む等 — 再発したら同じ手順で op を採取する

### D. メモ/エージェント (時計↔電話)

- **メモ録音が開始できない**: watch のメモアプリで録音開始ボタンを
  押しても反応なし。AIアプリでも「録音を開始できませんでした」。
  `CONFIG_WATCH_DISABLE_AUDIO=y` (スピーカー非搭載個体) の影響で
  録音パスごと切られている可能性 — 録音 (マイク) と再生
  (スピーカー) の依存を分離する必要があるか確認。マイク自体が
  非搭載なら UI から録音導線を消す判断も
- **電話側は読み込み専用**: メモ本文の取得は可だが、
  作成/編集は時計側のみの想定だったはず。仕様確認の上で
  電話→時計の書き込み経路が要るか検討

### E. デザイン系 (時計側)

- **テーマシステムと全体的なデザイン改善**: `docs/theme-format.md`
  がフォーマット定義、実行時適用は `components/ui/src/theme.cpp` /
  `theme_manager.cpp` / `theme_res.cpp`、アセット実体は
  `components/theme_store/`。cosmos/cyber/cute の整い方・拡張性・
  パッケージ資産の整理を含めてレビューして改善
- **テーマの自由度不足**: 現状テーマは「色だけ変えて共通レイアウト」
  なので表現が限られる。テーマが与えた色以外の色・自由な配置・
  画像の差し替えもできるよう、例外許容の仕組みを検討
- **小さいボタン等でテーマが崩れる**: サイズ違いの部品で
  テーマスタイルが破綻する箇所を洗い出して修正
- **数字は等幅に**: 時刻・タイマー・ストップウォッチなどの数字は
  tabular/monospace 系にして桁ぶれを防ぐ
- **ベゼルにUIがかぶる問題 (特に角)**: safe-area の機械値
  (kPad=32/mascot=36) は入っているが、まだ角が欠ける箇所あり。
  全画面総ざらい
- **ストップウォッチのデザイン**: 数字が小さすぎる。大きく見やすく
- **アプリアイコン**: applist 等にアプリごとのアイコンが欲しい
- **フォント**: `font_jp_20` / `font_jp_26` しかない。文字が小さく
  細くて読みにくい。サイズ展開とウェイトの追加
- **safe-area padding 最終値**: kPad=32 / mascot clamp=36 は機械的
  反映済み。各画面の余白バランスはデザイン判断
- **色・レイアウト統一感**: 全画面を通したトーン統一
- **描画負荷を意識したデザイン**: shadow/glow・gradient・AAテキスト
  は画素単位コストを積む。重い効果を軽い代替に置き換える判断も
  このタスクの範囲
- パーツギャラリー画面 (screenshot の 77 相当) で各ウィジェットの
  見え方を確認

### F. タイマー/アラーム通知 (時計側)

- **スピーカー非搭載で音が出せない**: 鳴動時は画面を強烈に発光
  (全面フラッシュ等) させる代替通知が要る。alarm/memo の
  ringing 状態で画面駆動する経路を作る

### G. スマホアプリ側

- **「通知・メディア」タブが重い**: 開くときにインストール済み全
  アプリをスキャンしている (installedApps)。遅延読み込み/キャッシュ
- **転送アプリが多すぎて探せない**: 通知転送アプリ選択に検索/フィルタ
- **文字が小さく細い**: 電話側 UI の可読性も
- **できることを増やす**: 現状は hello/time.set/settings/get + メモ・
  アラーム・通知転送・メディア・テーマ転送。さらなる操作 (時計側
  設定の完全編集・デバッグ情報表示・BLE/Wi-Fi 電波スキャン等の
  「ガチデバッグ」機能) の構想
- **BLE 切断耐性**: テーマ送信 (BULK) の途中で切れると最初から
  やり直し。transferId+offset で再開できるよう、BULK のレジューム
  対応 (時計側 bulk_in の next offset 応答を活かす)

### H. ツール・作りやすさ

- **テーマ作成を GUI で**: トップ画像や色をプレビューしながら
  作れるツール (PC or スマホアプリ内)。現在はパッケージを手で
  作る必要がありハードルが高い

## 未解決の技術課題

### 1. ~~「チラチラ」~~ → 解決済み

原因は **perf overlay の 300ms 自己再描画** (LOG_MODE ビルドで消失を実機確認)。
コード側に待機中の無駄更新は存在しない (redraw_cnt=0)。
`sdkconfig.defaults` で `LV_USE_PERF_MONITOR_LOG_MODE=y` を既定化済み
(overlay 非表示・UART に `sysmon:` 出力)。

### 2. 描画の重さ — 大幅改善済み

**根本修正**: SPI2 完了割り込みがデフォルト (低レベル) で、タッチ押下中に
同レベル割り込みの占有により ~13.5ms 遅延 → LVGL の全 flush が ~14ms
量子化していた。`buscfg.intr_flags = ESP_INTR_FLAG_LEVEL3`
(BSP esp32_s3_touch_amoled_2_06.c) で解消し、指押下中のスクロールは
「すごくよい」とユーザー確認済み。

併せて導入: タッチ I2C 読取りの専用タスク化＋影キャッシュ
(FT5x06 の ~14ms ブロッキング読取りを LVGL タスクから分離)、
`LV_DRAW_THREAD_PRIO=4`、静的リストの `bake_scroll` スナップショット化、
IMU サンプル削減、CPU 240MHz 固定。

残りの体感「引っかかり」は SW レンダの構造コスト: 全面 redraw 1回
~50–190ms → 実効 FPS ~5–6 が上限。**config レベルの加速手段は実機で
全て棄却済み**:
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

### 5. ~~BLE ペアリング/接続~~ → 全解決済み

実機で Android アプリとのペアリング・ボンド・再接続即暗号化・
`time.set` 時刻同期まで確認済み。見つかったバグ4件:

1. notify専用特性の `access_cb=nullptr` → NimBLE `ble_gatts_chr_is_sane`
   で EINVAL。全特性に access_cb 必須 (notify専用でも no-op が要る)
2. `CONFIG_BT_CTRL_BLE_MAX_ACT=1` → connectable 広告に activity 2枠
   必要で HCI `MEM_CAPACITY` (519)。2 に変更
3. **`ble_store_config_init()` 未呼出** → ボンド永続化ストア未登録で
   NimBLE が全 Pairing Request を ENOTSUP 拒否 (app=8 smerr=8)。
   `ble_link.cpp` で前方宣言して呼出 (公式 bleprph と同様にヘッダ非公開)
4. `CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=4096` → ボンド NVS 書込みが
   nimble_host 上で走り stack overflow → 毎接続で再起動。8192 に拡大

併せて: 接続時に時計側から `ble_gap_security_initiate` (peripheral 側
セキュリティ要求。電話主導の createBond に依存しない)、BLE 失敗パスへ
エラーログ追加。

### 4. その他留意

- **Android 通知アクセス**: adb サイドロードアプリは Android 13+ の
  「制限された設定」で通知アクセスONがブロックされる。設定→アプリ→
  SAS-Watch コンパニオン→⋮→「制限された設定を許可」で解除。
  MediaBridge (メディア操作/曲情報) もこの許可が無いと全く動かない
- deep sleep → PWR 復帰はフルリブート (本質的に遅い)。入る頻度を減らすなら
  `deep_sleep_after_s` を上げる (即時反映済み) のが確実な手。PM lock で
  light sleep 抑止は wake 遅延への効果が薄い (light sleep 復帰は元々速い)
- 音声関連: この個体は `CONFIG_WATCH_DISABLE_AUDIO=y` で全スキップ。
  littlefs `/storage` マウントは audio init 内で行われるが無効化時も残る
- 画面OFF中の広告 (旧 C案): 停止ではなく低頻度化
  (`ble_link_set_adv_rate`, ~1.0–1.3s)。一度切れると電話が
  再接続できず通知が届かないため。消費電流は実機で要確認

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
