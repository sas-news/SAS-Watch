# perf.md — 描画パイプラインと性能計測

描画まわりを変えるときは、ここの `b` ベンチで before/after を取ってから入れる (AGENTS.md)。

## 計測: シリアル `b` ベンチ

書き込み後、モニタ (USB-Serial/JTAG) に `b` を送る。Home / アプリ一覧 / 設定 /
ストップウォッチ / タイマー / 通知へ順に遷移し、画面ごとに 1 行出す:

```
bench: screen=settings full avg=56 max=56 ms scroll(n=32) avg=75 max=77 ms int_free=543 int_largest=144 psram_free=980068 psram_largest=966656
bench: skinstats: screen=settings blit(n=724 px=4802848) miss(n=157 px=6796798) rej(size=69 room=0 346x344<-306x101) fill=69 band=1932 imgreg=816 bail(fmt=0 scan=0 alpha=0 nonuni=276 runs=0 bands=0)
```

- `full`: 画面全体を無効化して `lv_refr_now` した時間 (描画 + 転送) を 6 回平均。
- `scroll`: スクロールできる列を 12px ずつ 16 回下へ、16 回上へ動かしたときの 1 フレームの時間。
- 後ろの 4 つはその画面を表示中のヒープ (内蔵 SRAM / PSRAM の空きと最大連続ブロック)。
- `skinstats` (ベンチ区間のスキン描画内訳、累計):
  - `blit` = キャッシュ済み完成形の等倍貼り付け (呼数/画素数)
  - `miss` = キャッシュに無くて 9-slice 伸縮描画した回数/画素数
  - `rej` = キャッシュ要求の拒否 (`size` は kSkinCacheMaxPx=32768px 超過で
    一生キャッシュされない部品、`<表示サイズ<-中心領域ソース>` でどの部品か分かる。
    `room` は予算 512KB が足りず構築を見送ったもの)
  - `fill` = 近似塗りで描いた領域数、`band` = そのとき発行した矩形 draw call 数
  - `imgreg` = 画像描画 (lv_draw_image) に戻った領域数
  - `bail` = 近似を断った理由 (fmt=非 RGB565(A8), scan=領域>640px, alpha=α 不均一,
    nonuni=直交方向が一様でない, runs=帯>64, bands=非ベース帯>40)
  - 注意: LVGL 9 の描画はタスク化されているので「呼数」は実描画時間ではない。
    帰属には `z` (スキン無効化) と照合する。
- `t` でテーマを順に切り替えられる (standard→mame→cosmos→cute→cyber。画面 OFF 中の `t` は
  タッチと同じく「起こすだけ」で処理されないので、画面が点いているときに送る)。
- `z` でスキン描画を段階的に切れる (0=全部 → 1=背景画像なし → 2=画面スキン全無し →
  3=部品スキンも無し → 0に戻る)。どこに時間があるかの帰属に使う。
- 実装: `firmware/components/ui/src/esp_display.cpp` の `perf_bench`。LVGL タスク上で走る。
- 画面遷移ごとの初回描画時間は従来どおり `perf: screen=<名> render=<ms>` 行に出る。

## 現在のパイプライン (2026-11-19 / T58 後)

```
LVGL (PARTIAL, double_buffer=true, 410x126 × 2 枚 = PSRAM ~202KB, LV_COLOR_FORMAT_RGB565)
  │  LVGL task (core1, prio 4, 9216B stack) がバンド i+1 を描画 / LV_DRAW_SW_DRAW_UNIT_CNT=1
  ├─ flush: band_flush_cb → xQueueSend(s_bands) で即 return
  ├─ wait : band_wait_cb (s_band_done セマフォ待機。スピンしない)
  └─ lcd_send task (core1, prio 5)
       1 バンドにつき CASET/RASET 1 回 → チャンク単位で copy_swap_rgb565 (32bit 1 パス)
       → wire_idle(1) で前 DMA 完了保証 → 内蔵 DMA バウンス 8.2KB × 2 へ 2Ch/3Ch 送出
       → on_chunk_sent ISR が s_chunk_free 返却 + 最終チャンクで flush_ready + portYIELD_FROM_ISR()
```

- LVGL タスク・`lcd_send` タスク・QSPI の SPI ISR は **core1 に固定** (`esp_display.cpp` の `task_affinity` / `lcd_send` core1、`firmware/vendor/esp32_s3_touch_amoled_2_06` の `buscfg.isr_cpu_id`)。別コアだと `spi_bus_lock` 競合 (IDFGH-17166) で凍る。BLE / Wi-Fi は core0。
- パネル制御 (`display_power` の `0x10`/`0x11` と `brightness_apply` の `0x51`) も `lcd_send_run()` 経由で `wire_idle(0)` 後に直列送出する。
- タッチとフレーム更新は `CONFIG_LV_DEF_REFR_PERIOD=15` (66Hz)、スタイル解決キャッシュは `CONFIG_LV_OBJ_STYLE_CACHE=y` (PSRAM ヒープ) を有効化。
- L1 キャッシュは `CONFIG_ESP32S3_DATA_CACHE_64KB=y` + `CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y` + `CONFIG_ESP32S3_INSTRUCTION_CACHE_16KB=y`。内蔵 SRAM 確保のため `CONFIG_ESP_WIFI_IRAM_OPT=n` / `CONFIG_ESP_WIFI_RX_IRAM_OPT=n` / `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=8` / `CONFIG_SPI_FLASH_SUPPORT_MXIC_OPI_CHIP=n` を併用。
- バンド待機は 1.5 秒 / スロット確保は 1 秒でタイムアウトし、`band_failed()` で復帰して UI ごと凍らせない。
- テーマの PNG は `png_stream.cpp` が行単位で展開する (作業メモリ ~50KB、ESP32-S3 ROM の tinfl)。
  全ピクセル不透明なら RGB565、透過ありなら RGB565A8 にする。対応外の PNG だけ lodepng に落ちる。
- **LVGL のヒープは PSRAM** (`lv_mem_psram.c` で `lv_malloc_core` 等を `heap_caps_malloc(MALLOC_CAP_SPIRAM)`
  に置き換え、`LV_USE_STDLIB_MALLOC=LV_STDLIB_CUSTOM`)。small alloc が
  `SPIRAM_MALLOC_ALWAYSINTERNAL` で内蔵に置かれて空きが数百B まで削っていたのを解消
  (設定画面表示中 ~0.5KB → ~20.9KB)。
- スキン (9-slice) は (画像, 幅, 高さ) ごとに完成形を 1 回だけ合成してキャッシュし、以後は等倍で貼る
  (`components.cpp` のスキンキャッシュ。最大 512KB、1 面の上限 32768px)。
  キャッシュの合成は ARGB8888 の canvas で行い (LVGL 9.5 の sw blend は RGB565A8 の
  描画先 α を書かないため)、全画素不透明なら RGB565、透過ありなら RGB565A8 で保持する。
  `lv_snapshot` は RGB565A8 非対応なので bake (アプリ一覧) は ARGB8888 のまま。

## 計測結果

### 描画パイプラインの変更 (standard テーマ、単位 ms)

| 画面 | 前 full | 後 full | 前 scroll | 後 scroll |
|---|---|---|---|---|
| Home | 112 | 40 | — | — |
| アプリ一覧 | 85 | 68 | 87 | 70 |
| 設定 | 104 | 56 | 128 | 74 |
| ストップウォッチ | 88 | 44 | — | — |
| タイマー | 91 | 55 | — | — |
| 通知 | 65 | 44 | — | — |

起動直後の Home 初回描画: 115ms → 40ms。内蔵 SRAM は起動時に +8KB 空いた
(描画バッファ 2 枚 + 使っていなかった回転用バッファ 1 枚 = 24.6KB → バウンス 2 枚 16.4KB)。
PSRAM は描画バッファ分の 402KB を使う。

### スキンキャッシュ + 領域近似塗り (同じ新パイプライン上、単位 ms)

| 画面 | cute 前 | cute 後 | cyber 前 | cyber 後 |
|---|---|---|---|---|
| Home | 96 | 80 | 104 | 85 |
| アプリ一覧 | 116 | 117 | 174 | 120 |
| 設定 full / scroll | 246 / 287 | 141 / 167 | 278 / 321 | 157 / 185 |
| ストップウォッチ | 120 | 98 | 114 | 103 |
| タイマー | 243 | 133 | 267 | 144 |
| 通知 | 120 | 92 | 126 | 96 |

「前」はスキン 9-slice を全部ソフト変換描画していた頃 (スキンキャッシュ導入前)。
standard は 42-56ms、mame も同程度なので無関係なし。内蔵 SRAM 空きは PSRAM 化で
~0.5KB → ~18-21KB に安定化。

スキン描画を丸ごと止めると (`z`=3) cute/cyber の全画面は ~40ms になる。残りの差は、
キャッシュに載らない部品 (大きなカード ≈346x344px、値で幅が変わるスライダー塗り) と、
縁ストリップの画像描画に残る部分。
予算を 1MB にすると設定は 164ms まで下がるが、アプリ一覧の bake (~850KB) が確保できなくなり
一覧が逆に遅くなる (116→174ms) ので 512KB にしている。

### 領域近似塗り (fill/band) の仕組み

大きい非コーナー領域 (>=2048px) では、ソース画素を 1D 走査してから描き方を決める
(`components.cpp` の `skin_1d_scan` / `skin_runs_draw`):

- **単色** → 矩形塗り 1 発 (cute のカード中央は誤差 0)。
- **線形グラデ** (走査線が端点 lerp に近い) → LVGL 2-stop グラデ矩形。
- **縞/帯** (cyber のスキャンライン等、period-4 の 2 色繰り返し) → 最多出現色を全面に
  敷き、残りの帯だけ重ねる (全帯を塗るより draw call 半減)。非ベース帯 >40 は画像に戻す。
- それ以外 (α 不均一、直交方向に非一様、領域 >640px) → そのまま画像描画。

走査軸は領域の形状から推し (横長 → 横走査)、だめなら両辺 >=64px の領域だけ逆軸も試す。
細い縁ストリップは LVGL の伸縮補間そのものが見た目の一部 (cute の端フェード) なので
近似しない — 実際、縁を強引に近似すると cute で max=146 の誤差が出た。
判定はすべて実画素ベースなので、近似できない絵は必ず画像に戻る (安全側)。

sim では環境変数 `SKIN_FILL_DEBUG=1` で領域ごとの判定を、`SKIN_FILL_OFF=1` で
近似を切って画像描画に戻せる (ESP では無効)。sim のスクリーンショットは
docs/screenshots の基準と比べて、cute は ~0 差分、cyber は縞の補間差のみ (max<=17)。

### T58 非同期 2 バンド転送 + 64KB D-Cache + 安定化修正 (2026-11-19、単位 ms)

同一実機 (`/dev/ttyACM0`, `on` ビルド) で 4 段階を `b` ベンチ比較 (Before は T58 直前 `0x2385c0`、Stage A は非同期 2 バンド + `REFR=15` + `STYLE_CACHE` + 64B ライン、Stage B1 は +64KB D-Cache + `WIFI_IRAM_OPT=n` + `STATIC_RX=8` (`CNT=1`)、Stage B2 は実験として `CNT=2` を計測、**最終**は `CNT=2` が LVGL 9.5 圧縮フォントの `font_fmt_rle` 非スレッドセーフでアイコン崩れを起こすため `CNT=1` へ戻し、`mol`/`sopia` 実機不具合の修正を含めた最終版 `0x238410`。表の「一覧 / 設定」は `full avg (scroll avg)`、他画面は `full avg`):

| テーマ / 段階 | Home | 一覧 (`scroll`) | 設定 (`scroll`) | SW | タイマー | 通知 | 起動後内部 `heap` / `int_free` / `int_lrg` | Home `psram_free` |
|---|---|---|---|---|---|---|---|---|
| `standard` 前 | 31 | 68 (71) | 51 (74) | 43 | 52 | 41 | 90,743 / 98,395 / 31,744 | 4,161,576 |
| `standard` Stage A | 22 | 54 (55) | 41 (61) | 35 | 41 | 31 | 89,643 / 97,295 / 31,744 | 4,370,260 |
| `standard` Stage B1 (`CNT=1`) | 21 | 52 (54) | 40 (60) | 35 | 40 | 30 | 78,203 / 85,855 / 36,864 | 4,304,628 |
| `standard` 実験 `CNT=2` (`*2`) | 21 | 49 (51) | 39 (60) | 35 | 37 | 30 | 69,643 / 77,295 / 31,744 | 4,304,572 |
| **`standard` 最終 (`CNT=1`)** | **20** | **53 (54)** | **40 (60)** | **35** | **40** | **30** | **76,139 / 83,791 / 34,816** | **4,304,600** |
| `mame` 前 | 31 | 54 (69) | 51 (74) | 43 | 52 | 41 | 同上 | 983,120 (`lrg=638,976`) |
| `mame` Stage A | 22 | 43 (57) | 41 (61) | 35 | 41 | 30 | 同上 | 1,191,716 (`lrg=835,584`) |
| `mame` 実験 `CNT=2` (`*2`) | 22 | 41 (56) | 39 (60) | 34 | 37 | 30 | 同上 | 1,126,040 (`lrg=770,048`) |
| **`mame` 最終 (`CNT=1`)** | **21** | **42 (56)** | **40 (59)** | **35** | **39** | **30** | **同上** | **1,126,120 (`lrg=770,048`)** |
| `cosmos` 前 | 91 | — (`*1`) | 77 (99) | 78 | 77 | 78 | 同上 | 1,527,928 |
| `cosmos` Stage A | 75 | 67 (69) | 65 (85) | 63 | 85 | 60 | 同上 | 1,736,492 |
| `cosmos` 実験 `CNT=2` (`*2`) | 72 | 56 (57) | 59 (80) | 59 | 75 | 56 | 同上 | 1,670,816 |
| **`cosmos` 最終 (`CNT=1`)** | **76** | **66 (68)** | **64 (84)** | **63** | **83** | **60** | **同上** | **1,670,844** |
| `cute` 前 | 70 | — (`*1`) | 164 (198) | 73 | 133 | 66 | 同上 | 1,507,088 |
| `cute` Stage A | 52 | 94 (96) | 121 (149) | 81 | 116 | 69 | 同上 | 1,715,728 |
| `cute` 実験 `CNT=2` (`*2`) | 49 | 77 (80) | 104 (130) | 70 | 99 | 61 | 同上 | 1,650,052 |
| **`cute` 最終 (`CNT=1`)** | **53** | **93 (95)** | **119 (147)** | **81** | **116** | **70** | **同上** | **1,650,080** |
| `cyber` 前 | 75 | — (`*1`) | 184 (222) | 102 | — (`*1`) | 94 | 同上 | 1,507,060 |
| `cyber` Stage A | 57 | 98 (100) | 140 (169) | 85 | 130 | 75 | 同上 | 1,715,700 |
| `cyber` 実験 `CNT=2` (`*2`) | 52 | 81 (83) | 118 (146) | 74 | 109 | 65 | 同上 | 1,650,032 |
| **`cyber` 最終 (`CNT=1`)** | **57** | **98 (100)** | **138 (166)** | **86** | **129** | **75** | **同上** | **1,650,060** |
| **`mol` 最終 (`CNT=1`)** | **98** | **90 (92)** | **70 (89)** | **73** | **99** | **75** | **83,807 / 34,816 (`*3`)** | **2,114,116 (`lrg=1,769,472`)** |
| **`sopia` 最終 (`CNT=1`)** | **170** | **83 (85)** | **70 (89)** | **64** | **88** | **65** | **83,807 / 34,816 (`*3`)** | **1,533,920 (`lrg=1,245,184`)** |

`*1`: Before では重い画面計測中に 40 枠 `ActionQueue` が `ImuSample` で満杯となり `Navigate` がドロップしたため未取得 (`watch_app.cpp` の `'b'` ハンドラへ `wait_queue_drain` を入れて解消)。なお、Before の旧 `docs/perf.md` 記録では `cute` 一覧=117ms、`cyber` 一覧=120ms・タイマー=144ms。
`*2`: `CNT=2` は圧縮フォント展開のグローバル状態競合 (`lv_font_fmt_txt.c` の `font_fmt_rle`) により `font_icon_36` 等が崩れるため不採用 (`CNT=1` へ戻し)。
`*3`: 拡張テーマ (`y` 切替、`mol`/`sopia`) は同一ビルド上の個別 `b` ベンチ実測 (冷起動 `sopia` の `'h'` `boot_heap=76,155B`、`mol` 一覧中 `psram_free=1,027,016` / `psram_largest=704,512`、`sopia` 一覧中 `psram_free=446,056` / `psram_largest=200,704`)。

- **PSRAM 節約と `bake_scroll`**: 描画バッファを全面 1 枚 (402KB) から 126 行×2 枚 (~202KB) へ縮小して ~205KB の PSRAM を解放した結果、`cosmos` / `cute` / `cyber` / `mol` / `sopia` でもアプリ一覧の `bake_scroll` が安定成功するようになった (`cute` 一覧 93/95ms、`cyber` 98/100ms、`cosmos` 66/68ms、`mol` 90/92ms、`sopia` 83/85ms)。なお `mame` は `346×733` ARGB8888 (1,014KB) の連続確保は不成立だが非ベイクでも `42/56ms` と十分高速。
- **内部 SRAM と Wi-Fi/BLE 共存の実測知見**:
  - `64KB D-Cache` (-32KB 内部 SRAM) に `32KB I-Cache` (-16KB) や `WIFI_IRAM_OPT=y` (-17.7KB) を重ねると、起動後ヒープが 44.1KB〜61.8KB になり、Wi-Fi 起動時の RX DMA / 管理フレーム TX (`W wifi:m f probe req/auth/assoc req`) が内部 DMA SRAM 枯渇で失敗する。
  - `64KB D-Cache` + **`16KB I-Cache`** + **`WIFI_IRAM_OPT=n` / `WIFI_RX_IRAM_OPT=n`** + **`STATIC_RX_BUFFER_NUM=8`** + `MXIC_OPI=n` とし、`taskLVGL` スタックを `9216B` (+2KB、下記 `sopia` 対策) へ拡張しても、起動後ヒープ **`76,139B`** (`int_free=83,791B` / `int_largest=34,816B`) を確保。`ble_synced=1` 維持かつ `muse start` で Wi-Fi 接続 → `hatch.metaaivm.com` への mbedTLS/HTTP 往復 (`m f` エラー 0 件、ピーク TLS 中の `'h'` 観測値 `heap=17,099B`、`muse stop` 後 `62,079B` 回復) まで確認済み (実機上の M1 トークン期限切れによりサーバは HTTP 401/403 `Token rejected` を返却)。
- **T58 後に見つかった 3 件の実機不具合と対策**:
  1. **一部アプリアイコンの崩れ (`DRAW_UNIT_CNT=2`)**: LVGL 9.5 の圧縮ビットマップ展開 (`lv_font_fmt_txt.c`) が共有状態 `LV_GLOBAL_DEFAULT()->font_fmt_rle` を無ロックで使っており、組み込み圧縮フォント (`font_icon_36.c`) の並列描画で化けて `bake_scroll` に焼き付いていた。**`CNT=1` に戻して解消** (内部 SRAM も +8.6KB 回復)。
  2. **「星夜もる」(`mol`) トップがメニュー背景になる**: `More` 画面の `bake_scroll` 成功後に `Home` へ戻ると `psram_largest` が ~280KB まで下がり `chara1.png` (353KB) の一時読込に落ち、さらに `theme_image()` が `s_active` のみに書き込んで画面遷移の `s_active = s_base` で揮発・アリーナリーク、`ct=2` RGB PNG を 3B/px で確保していた問題が重なっていた。`ct=2` の 2B/px 見積り/確保 + アリーナ巻き戻し (`theme_asset_mark/rewind`) + `theme_cache_asset()` で `s_base` 同期 + 先頭 `chara*` 先読みを入れて解消 (`png_selftest` `'p'` 全 10 件 byte-exact `OK0` を確認)。
  3. **`sopia` / `mol` での `taskLVGL` スタック溢れ**: `face_theme.cpp` の影/上光/縁取り 4 枚の 7 段親子連鎖で `lv_obj_refr` 再帰だけで ~4.8KB を消費し、`esp_lvgl_port` 既定 `7168B` を超過していた。装飾層を先頭ルート直下にフラット化 (depth 2) + `task_stack = 9216` に拡張して解消 (`sopia` 冷起動を含め通過)。
- **サイズ (`off` / `on`)**: `off` バイナリ `2,213,200B` (`0x21c550`, 静的 DIRAM 空き `135,525B`)、`on` バイナリ `2,327,568B` (`0x238410`, 静的 DIRAM 空き `129,933B`)。

## 遅さの原因だったもの

1. **小さい描画バッファ** (内蔵 SRAM 410×10 行 ×2)。LVGL は 1 フレームを ~50 回に分けて描き、
   毎回オブジェクトツリーを辿り直し、クリップした図形やグリフ (圧縮フォントは展開も) を描き直していた。
   転送も 50 回の CASET/RASET。PSRAM の全面バッファにして 1 パスにしたのが一番効いた。
   PSRAM への描画は内蔵より遅いが、パスの回数が減る効果の方がずっと大きい。
2. **9-slice の伸縮描画**。辺と中央を `scale_x/y != 256` で描くので、LVGL のソフト変換
   (一時バッファ + 画素ごとの補間) を部品ごと・フレームごとに通っていた。
3. (副作用の修正) **全面 PNG のデコード**: lodepng は展開バッファと出力で ~1.65MB を同時に使う。
   全面描画バッファの 402KB と合わせると PSRAM が足りず、cosmos の背景が黙って消えていた。

## 試して比較・棄却したもの

| 手段 | 結果 |
|---|---|
| `LV_DRAW_SW_DRAW_UNIT_CNT=2` (2 コアで描画) | 同期パイプライン・T58 非同期+64KB D-Cache の両方で計測。T58 上で単発速度は 12〜18% 短縮 (`cute` 設定 119/147→104/130ms 等) したが、LVGL 9.5 の圧縮フォント展開 (`lv_font_fmt_txt.c` の `font_fmt_rle`) が無ロックの共有グローバル状態を使っており、`applist` の `font_icon_36` 等が 2 スレッド競合で崩れるため **却下** (`CNT=1` 維持・内部 SRAM +8.6KB) |
| `ESP32S3_INSTRUCTION_CACHE_32KB=y` (32KB I-Cache) | XIP コード実行 (`standard` 設定 40/60→36/53ms 等) には効くが内部 SRAM を追加 16KB 消費し、`64KB D-Cache` と併用すると Wi-Fi 起動・管理フレーム TX (`m f`) が SRAM 枯渇で失敗するため **却下** (16KB を維持) |
| `ESP32S3_DATA_CACHE_64KB=y` (64KB D-Cache) | 当初は内部 SRAM 32KB 消費で BLE init 落ち懸念があったが、`theme_store`/`esp_port`/`theme` の静的 PSRAM 退避 (+96.4KB) と `WIFI_IRAM_OPT=n` / `STATIC_RX=8` の併用により **BLE + Wi-Fi/TLS と両立できたため採用** |
| PPA | ESP32-S3 には無い (P4 のみ) |
| esp_lvgl_port の S3 SIMD | LVGL 9.1 専用。9.5 では使えない |
| `LV_DRAW_BUF_STRIDE_ALIGN=64` | 効果なし |
| 全面バッファ 2 枚 (描画と転送の重ね合わせ) | PSRAM がさらに 402KB 要る。画像テーマで足りないので見送り (T58 では **126 行 × 2 枚 = 202KB** に縮小して重ね合わせを実現) |

## 落とし穴

- **spi_bus_lock のマルチコア競合** (esp-idf #18177 / IDFGH-17166、ESP-Techpedia の SPI/QSPI 画面 FAQ)。
  LVGL タスクと SPI ISR が別コアだと、`spi_device_acquire_bus` が誰も持っていないロックを永久に待ち、
  LVGL タスクがロックを握ったまま凍る (gdb で確認)。旧パイプラインでも潜在していた。両方 core1 に固定して解消
  (固定後、5 テーマで 41 回連続ベンチして再発なし)。
- **画面削除のタイマー**: 遷移後の旧画面は `anim_ms+60` 後に消すが、LVGL タスクが重い処理で遅れると
  遷移アニメの完了より先に走り、`disp->prev_scr` が解放済みを指したまま次の描画で落ちていた。
  アニメが残っている間は削除を延期する (`shell.cpp` の `del_old_screen`)。
- **PSRAM の残り**: T24 の binfont 常駐 (~1.4MB) 後は、肌テーマで空き ~750KB /
  最大連続 ~520KB、mame で空き ~450KB / 最大連続 ~200KB まで減る。
  常駐する確保を足すときは全テーマで `b` を回して `psram_free` / `psram_largest` を見る。
- **binfont のメモリ** (T24): `lv_binfont_create_from_buffer` はグリフ表・cmap・
  bitmap を全部 lv_malloc に複製する (ソースバッファは create 後に解放可)。
  テーマアリーナより先に確保しないとアリーナと PSRAM を取り合って OOM
  アサート→LVGL タスク停止→WDT になる (実機で観測)。`ui::init_display` で
  プリロード + 最大ブロックの事前チェックで回避した。アリーナも
  資産領域と画面ブロック (640KiB×2) を別個確保に変えた
  (単一 ~3MB はフォント常駐後の分断で連続確保できない)。
- **アプリ一覧 bake の連続確保不足**: 肌テーマで最大連続 ~500KB < bake ~850KB
  → bake はほぼフォールバック (見た目同じ、スクロール ~180ms)。失敗時は
  スキンキャッシュ (最大 512KB) を捨てて一度だけ再試行する。
  ARGB4444/8565 への縮小は画像ソースとして描けず却下 (全面黒)。
- **内蔵 SRAM の残り**: LVGL ヒープを PSRAM に移して解消済み (表示中 ~18-21KB 空き)。
  以前は `SPIRAM_MALLOC_ALWAYSINTERNAL=1024` で LVGL の小さいオブジェクトが内蔵に
  置かれ、設定画面で空きが数百バイトまで削れていた。DMA/ISR 用の確保は今も内蔵が必要。
- **FT5x06 の I2C NACK**: タッチ IC (FT3168) は無操作時に省電力スリープし、100ms の
  セーフティポーリングが必ず NACK になる。`esp_lvgl_port_touch.c` を vendored して、
  INT ピン (GPIO38) がアサートされていない限り読み出さないようにした
  (NACK ログ消滅。INT 取りこぼしはレベルが残るのでカバー済み)。
- **`swap_xy` 警告**: vendor の `esp_lvgl_port_disp.c` が表示回転の度に呼んでいた
  (このボードでは回転なし)。パッチ済みで起動ログが静かになった。
