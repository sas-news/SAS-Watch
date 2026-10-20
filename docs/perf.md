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

## 現在のパイプライン (2026-10-19)

```
LVGL (PARTIAL, 410x502 の描画バッファ 1 枚 = PSRAM 402KB)
  └─ flush: bounce_flush_cb
       行単位で内蔵 SRAM の 8.2KB バウンスバッファ 2 枚へコピー + RGB565 バイトスワップ
       → esp_lcd_panel_draw_bitmap (QSPI 80MHz, DMA)
       完了 ISR がスロットを返すので、チャンク i の転送中に i+1 をコピーする
```

- LVGL タスクと QSPI の SPI ISR は **core1 に固定** (`esp_display.cpp` の `task_affinity`、
  `firmware/vendor/esp32_s3_touch_amoled_2_06` の `buscfg.isr_cpu_id`)。
- バウンスのスロット待ちは 1 秒でタイムアウトし、ログを出してそのフレームの残りを捨てる
  (完了 ISR が来ない異常で UI ごと凍らないため)。`bench timeout` の行に flush の入出カウンタが出る。
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

## 遅さの原因だったもの

1. **小さい描画バッファ** (内蔵 SRAM 410×10 行 ×2)。LVGL は 1 フレームを ~50 回に分けて描き、
   毎回オブジェクトツリーを辿り直し、クリップした図形やグリフ (圧縮フォントは展開も) を描き直していた。
   転送も 50 回の CASET/RASET。PSRAM の全面バッファにして 1 パスにしたのが一番効いた。
   PSRAM への描画は内蔵より遅いが、パスの回数が減る効果の方がずっと大きい。
2. **9-slice の伸縮描画**。辺と中央を `scale_x/y != 256` で描くので、LVGL のソフト変換
   (一時バッファ + 画素ごとの補間) を部品ごと・フレームごとに通っていた。
3. (副作用の修正) **全面 PNG のデコード**: lodepng は展開バッファと出力で ~1.65MB を同時に使う。
   全面描画バッファの 402KB と合わせると PSRAM が足りず、cosmos の背景が黙って消えていた。

## 試して棄却したもの

| 手段 | 結果 |
|---|---|
| `LV_DRAW_SW_DRAW_UNIT_CNT=2` (2 コアで描画) | 旧パイプラインでは悪化。新パイプラインでも改善なし (Home 41→40ms) で、内蔵 SRAM が 8.5KB 減る |
| PPA | ESP32-S3 には無い (P4 のみ) |
| esp_lvgl_port の S3 SIMD | LVGL 9.1 専用。9.5 では使えない |
| `LV_DRAW_BUF_STRIDE_ALIGN=64` | 効果なし |
| データキャッシュ 64KB | 内蔵 SRAM を 32KB 食うので試していない (BLE init が落ちる) |
| 全面バッファ 2 枚 (描画と転送の重ね合わせ) | PSRAM がさらに 402KB 要る。画像テーマで足りないので見送り |

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
