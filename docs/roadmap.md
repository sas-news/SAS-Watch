# roadmap.md — 残タスクの実施計画 (引き継ぎ用。2026-10-19 更新)

残タスク全部について**方針はすでに決めてある** (元は 2026-10-12 の handoff の残タスク一覧。
旧 handoff は `docs/archive/handoff-2026-10-15.md`)。
どのモデルが引き継いでも、ここに書いた方針・受け入れ条件どおりに進めればよい。
方針を変えたいときは、理由をここに追記してから変えること。

- 進捗は各タスクの `状態:` 行を更新する (未着手 / 作業中 / 完了 <commit>)。
- 「難度」の意味: **S** = 方針どおりの機械作業 (どのモデルでも可)、
  **M** = 実装に設計の細部が絡む (方針の範囲内で判断してよい)、
  **H** = 新たなデザイン判断が要る (迷ったらユーザーに確認する)。
- 「実機」= 書き込み後にユーザーの体感確認が要るもの。
  ユーザーに依頼するときは、確認手順を箇条書きで渡す。

## 共通ルール

- AGENTS.md のルールを守る (core/ は ESP-IDF/LVGL 非依存、C++17、例外/RTTI なし、
  推測値には `// TODO(hw): 実機で確認`、ユーザー向け文字列は日本語)。
- 検証手段:
  - core: `cmake -S core -B core/build && cmake --build core/build && ctest --test-dir core/build`
  - sim (デザイン確認の主手段): `cmake -S sim -B sim/build && cmake --build sim/build -j && ./sim/build/snapshot --out sim/screenshots`
    で全画面の PNG が出る。**デザイン系の変更は、変更前後のスクショを並べて確認する**。
  - firmware: `cd firmware && IDF_CCACHE_ENABLE=1 idf.py build`
    (ESP-IDF は `/home/sasnews/esp/esp-idf`)。書き込みは firmware/README.md「書き込み・ログ」の
    esptool 手順 (アプリは 0x20000)。描画性能は `b` ベンチ (docs/perf.md)。
  - android: `cd android && ./gradlew :protocol:test :agent:test :app:assembleDebug :app:lint`
    (JAVA_HOME = mise の temurin-17、SDK = `~/android-sdk`)。
    アプリ側のユニットテストは `:app:testDebugUnitTest`。
- 内蔵 SRAM は逼迫している (起動直後で空き ~141KB。BLE init には 32KB の連続領域が要る)。
  大きいバッファは PSRAM に置く (`MALLOC_CAP_SPIRAM`)。
- flash の app 領域の空きは ~1.39MB。フォントや画像を内蔵するときはサイズを報告する。
- 安全域 (safe-area): パネルの角は R≈100px。矩形の角点 (x,y) は角の円の内側に
  余裕 6px 以上を残すこと。左右の余白は kPad=32 が基準。
  判定は T0 の `safe_area` 関数を使う。

---

## T0. 共通基盤 (他タスクの前提) — 難度 M

1. **safe-area 判定関数** (core, 純粋関数 + テスト):
   `core/include/watch/safe_area.hpp` に
   `bool rect_in_safe_area(int x, int y, int w, int h, int margin = 6)`
   (画面 410x502、角 R=100) を置く。4隅について
   「角点が R の円弧より内側に margin 以上入っているか」を判定する。
   ホストテストは境界値で書く。
2. **sim の安全域監査**: snapshot の各画面で LVGL のオブジェクトツリーを巡回し、
   `HIDDEN` でなく、テキストを持つか CLICKABLE のオブジェクトの座標を
   `rect_in_safe_area` で判定する。違反を `sim/screenshots/safe_area_report.txt` に
   「画面名 / オブジェクトの種類 / 座標 / テキストの先頭」で出力する。
   装飾 (画像・背景) は対象外。
   CI では当面 fail にせず、レポートを artifact に含めるだけにする。
3. **性能計測フック**: firmware の LOG_MODE の sysmon ログに出ている
   render/flush の時間を、画面遷移ごとに `perf: screen=<name> render=<ms>` として
   1行出す (最初のフル描画のみ)。T5 の計測に使う。
4. **sim のスクショをテーマ別にも出す**: `--theme <id>` オプション
   (内蔵 standard/light + `sim/themes/*.zip`)。

状態: 完了 91c34b3

## T1. ベゼル角欠けの総ざらい (E) — 難度 S (T0 の後)

- T0-2 のレポートの違反をゼロにする。棚卸しで既に分かっている違反:
  - 通知トースト `(10,8)` 幅390 → `x=32, y=36` 幅346 (上端の角を避ける)
  - alert ボタン 幅366 → 幅346 (= 410−2·32)
  - face_hud `s.en` TOP_LEFT 44px → 安全域内に移す
  - face_theme の `face_layout` 座標 → 適用時に `rect_in_safe_area` で
    内側へクランプする (立ち絵 `chara` は装飾なのでクランプしない)
  - mascot の原寸表示クランプ 22 → 36 (docs/theme-format.md の記述も 22→36 に直す)
- face_chara_* の立ち絵は装飾なので、角で切れてよい。
- 受け入れ条件: レポートの違反 0 件。全画面スクショで文字・ボタンが角で欠けていない。

状態: 完了 8463e18

## T2. 小さい部品でテーマが崩れる問題 (E) — 難度 M

- 部品の寸法を相互に導出する形に直す (ハードコードの組み合わせをやめる):
  - switch: track 52x30。knob = track 高さ − 2·pad (pad 3) = 24。outline 4 は廃止して
    knob の影を使わない。ON/OFF は色だけで表す。
  - slider: track 10、knob 24 のまま。knob の当たり判定は ext_click_area で 48 確保
    (tap_min 48 のルール)。
  - ボタン: 幅は親の content 幅に従う (固定 366 をやめる)。ラベルは
    `LV_LABEL_LONG_DOT`。radius は `min(style.btn_radius, h/2)`。
  - card/row: radius を `min(r, h/2)` でクランプする。glow は T5 の上限に従う。
- **パーツギャラリー**: sim に `gallery` 画面を追加する (実機ビルドからは除外)。
  全部品を通常 / 小 (ボタン h40 など) / 押下 / 無効 / ON の状態で並べ、全テーマでスクショを撮る。
- 受け入れ条件: 全テーマのギャラリーで、角丸のはみ出し・文字のはみ出し・knob のずれが無い。

状態: 完了 5c7ea72 (+cad6be4)

## T3. 等幅数字の残り (E) — 難度 S

- 文字盤用フォント `fc_*` (5系統 × 4サイズ) の数字が等幅かを確認する
  (各数字グリフの adv_w を比較するスクリプトでよい)。
  等幅でない系統は `tools/gen_fonts.py` で数字グリフの advance を最大幅に揃えて再生成する
  (lv_font_conv に等幅化の手段が無ければ、TTF の tnum feature を使うか、
  後処理で adv_w を書き換える)。
- 受け入れ条件: 秒が進んでも文字盤の時刻が横に揺れない (sim で連続スクショを比較)。

状態: 完了 d432d1f (+d3d46b6, +cad6be4)

## T4. フォント (E) — 難度 M

- 決定:
  - 本文フォントを **Medium ウェイト**の日本語フォントで作り直す。
    フォントは M PLUS 1p Medium か Noto Sans JP Medium (どちらも OFL)。
    ライセンス文を `tools/fonts/jp/` に同梱する。
    サイズは 20/26 のまま置き換えるので、flash の増加はほぼゼロ。
  - 見出し用に `font_jp_32` (Bold) を追加する。グリフは
    「ASCII + かな + UI ソース中の文字列リテラルに出てくる漢字」だけに絞る。
    `tools/gen_fonts.py --subset-from-sources` でソースから文字を抽出する。
    動的な文字列 (通知・曲名など) には使わない (欠け防止)。
  - 本文の行間は 1.25 倍程度にする。
- 適用: header のタイトルを 32 にする。row の主テキストは 26、
  副テキストは 20 の text_dim。
- 受け入れ条件: flash の増加を報告すること (目安 300KB 以下)。全画面スクショで文字の溢れが無い。

状態: 完了 defa083

## T5. 描画負荷を下げるデザイン + 滑らかさ (A, E) — 難度 M (計測は実機)

- 方針: **重い効果を内蔵テーマで既定 off にする。テーマ側では上限付きで使えるようにする。**
  - glow (shadow): 内蔵テーマでは 0。テーマ指定の `glow_w` は最大 6 にクランプする。
  - グラデーション: alert の radial grad は単色 + 細いリング 1 本にする。
    analog 盤の grad は単色にする。slider の grad は単色にする。
  - 画面遷移アニメ: `anim_ms` の既定を 0 (即時切替) にする。全面 redraw に
    50–190ms かかるので、スライドは紙芝居になるだけ。テーマで指定した場合も最大 150ms。
  - 秒の更新: 秒を出す文字盤は、毎秒の invalidate 範囲を秒表示の矩形に限る。
    analog の秒針は既定 off にし、設定で on にできるようにする。
  - スクロールリストの `bake_scroll` は今のまま。
- 計測: T0-3 の `perf:` ログで、主要画面の初回 render ms を変更前と変更後で表にして
  handoff.md に記録する (ユーザーに書き込みと操作を依頼)。
  → T29 の `b` ベンチ (docs/perf.md) に置き換え。
- 受け入れ条件: 主要画面の初回 render が変更前より短い。見た目の劣化はスクショで確認する。

状態: 完了 2a09e5f

## T6. 色・レイアウトの統一感 (E) — 難度 H (ルールは決定済み)

- 色の使い分けを次のルールで統一し、全画面スクショで監査して直す:
  - primary は「その画面の主操作」と「ON/選択中の状態」だけに使う。1画面に主操作は1つ。
  - danger は破壊的な操作 (削除・リセット) だけ。
  - 本文は text、補足は text_dim。それ以外の色を文字に使わない (文字盤は例外)。
  - 余白は space の倍数 (8/16/24/32) だけを使う。
  - ヘッダはどの画面も同じ形にする (タイトル左寄せ・戻るピル・右に mascot)。
- 監査結果 (違反一覧) は sim スクショ付きでユーザーに見せ、承認後に直す。

状態: T16 に吸収 (監査結果は docs/archive/t6-audit.md)

## T7. アプリアイコン (E) — 難度 S

- 仕組みは既にある (`theme_icon(app_id)`、テーマの `icons`)。足りないのは**内蔵の既定アイコン**。
- 決定: **Material Symbols Rounded** (Apache-2.0) から 10 アプリ分のグリフを
  `font_icon_36` (アイコンフォント、4bpp) にする。色は text_color で primary に染める。
  テーマの icons があればそちらを優先する。
- アプリと記号の対応: timer=timer、stopwatch=timer_play(無ければ avg_pace)、
  counter=exposure_plus_1、memo=mic、alarm=alarm、notifications=notifications、
  media=music_note、steps=directions_walk、agent=smart_toy、settings=settings。
- 受け入れ条件: applist のスクショで全アプリにアイコンが出る。flash の増加を報告する。

状態: 完了 3ade325

## T8. テーマの自由度を上げる (E) — 難度 M

- 現状 (v3) で既にできること: 画面ごとの背景、9-slice スキン、アイコン、フォント、
  マスコット、文字盤の配置。足りないのは「画面単位の色の上書き」と
  「装飾画像を自由な位置に置くこと」。
- 追加するキー (形式 v4。旧ファームは未知キーとして無視するので互換が保てる):
  ```jsonc
  "screens": {
    "timer": {
      "bg": "timer_bg.png", "scrim": 60,
      "tokens": {"primary": "0xFF3355", "text": "0xFFFFFF"},   // 画面単位の色の上書き
      "deco": [                                                // 装飾画像 (最大4枚/画面)
        {"img": "star.png", "x": 300, "y": 40, "z": "back"},   // z: back=内容の背面 / front=前面 (タップは透過)
        {"img": "ribbon.png", "x": -10, "y": -10, "z": "front"} // 負値は右端・下端基準
      ]
    }
  },
  "palette": {"pink": "0xFF77AA", "mint": "0x77FFCC"}  // face_layout などから "@pink" で参照できる名前付きの色
  ```
- 実装: 画面を生成するとき、その画面の token 上書きを一時的に適用した Theme を
  部品に渡す。deco は画面背景と同じ遅延ロードの領域から取る
  (640KiB × 2 ブロックの予算内)。face_layout の色指定 `@name` は palette を引く。
- core のマニフェストパーサにテストを足す (上書き・deco の上限・不正値)。
  docs/theme-format.md に v4 として追記する。
- サンプル: cosmos に timer 画面の上書き 1 つと deco 2 枚を足して、sim のスクショで確認する。

状態: 完了 722985a

## T9. 音の代わりの画面フラッシュ通知 (F) — 難度 S (実機)

- `shell.cpp` の alert (`show_alarm_alert` / `show_timer_alert`、`s_alert` で単一管理) を拡張する:
  鳴動中は全面の lv_obj (alert の内容の背面) を 500ms 周期で白と黒に切り替え、
  輝度を 100% にする。
  60 秒で点滅を止める (alert 表示とバイブは継続)。何か入力があれば即停止し、輝度を元に戻す。
- バイブは鳴動中 2 秒ごとに 600ms を繰り返す。
- 受け入れ条件: sim で点滅の 2 フレームを確認する。実機ではアラームを1分後に設定して鳴らす。

状態: 完了 bd99cbd

## T10. マイク/スピーカー (D) — 難度 M (実機)

- 事実: この個体は I2C scan で **ES8311 (0x18, codec) と ES7210 (0x40, マイク ADC) が応答する**。
  `CONFIG_WATCH_DISABLE_AUDIO=y` は録音と再生をまとめて切っている。
  無効化の本当の理由は、過去の I2S init の NO_MEM (内蔵 SRAM 不足) だった可能性が高い。
- 方針:
  1. Kconfig を `WATCH_ENABLE_MIC` と `WATCH_ENABLE_SPEAKER` の2つに分ける
     (`WATCH_DISABLE_AUDIO` は互換のため両方 off の意味として残す)。
  2. まずマイクだけを有効化する: ES7210 + I2S RX。DMA バッファは最小
     (例 4×240 フレーム) にし、録音中だけ確保する (起動時には確保しない)。
     BLE init の後に初期化する。
  3. 録音 UI は「マイクが使えるか」の実行時フラグで出し分ける。
     使えなければ録音ボタンを隠し、「この個体では録音できません」を出す。
  4. マイクが動いたら、次にスピーカーを同じ方式 (再生中だけ確保) で試す。
     出音はユーザーに確認してもらう。
     スピーカーが無い/鳴らない場合は ENABLE_SPEAKER=n のままにする。
- 受け入れ条件: 実機でメモを 5 秒録音して保存でき、電話から取得して再生できる。
  録音中も BLE が切れない。内蔵 SRAM の最小空きを報告する。

状態: 完了 1afff60

## T11. アルバムアート (C) — 難度 M (実機)

- wire 形式:
  - `media.state` に任意フィールド `art` (u32: 画像の識別子 = 電話側で計算した hash) を追加する。
    0 または省略なら画像なし。
  - 画像は BULK の `kind:"asset"`、name `media_art` で送る。中身は LVGL bin
    (RGB565, **120x120**, 28,812B)。電話側で MediaMetadata の ART/ALBUM_ART
    (無ければ DISPLAY_ICON) を中央切り抜きで縮小して作る。
    時計に同じ art id が既にあれば送らない。
- 時計側: `media_art` は**flash に保存しない** (書込み摩耗を避ける)。
  PSRAM のバッファ 1 枚に受けて、受信完了で `MediaArtUpdated` Event を出す。
  media 画面は art id が一致するときだけ画像を表示し、それ以外は既定の音符アイコン (T7) を出す。
  BLE が切断したら破棄する。
- protocol-v1.md と電話側のテストを更新する。
- 受け入れ条件: 実機で曲を変えると 2 秒以内にアートが替わる。

状態: 完了 d23d509

## T12. BULK のレジューム (G) — 難度 M

- 時計側の `bulk_in` は既に `BULK_ACK{next}` を返す。電話側の `BulkSender` が
  切断後に再開しているかを確認し、無ければ実装する:
  転送状態 (id, sha256, size, 確認済み offset) をメモリに持つ。
  再接続後に同じ id で BULK_START を送り、ACK の next から続きを送る。
  再接続が 3 回失敗するか、5 分経ったら諦める。
- 時計側で、切断しても部分ファイル (`/assets/.theme.bulk`) と受信状態が残るかを確認する。
  再起動を跨ぐ再開は対象外。
- 受け入れ条件: 電話側のユニットテスト (偽の接続で途中切断 → 再開) と、
  実機でテーマ送信中に時計の BLE を切る (電話の BT を一瞬 off) 試験。

状態: 完了 64e5e48

## T13. 電話アプリでできることを増やす (G) — 難度 M

- 決定 (BLE/Wi-Fi の電波スキャンは SRAM の制約が大きく、価値も低いのでやらない):
  1. 新しい op `debug.info` → 返す項目: 内蔵/PSRAM の空きと最小空き、稼働時間、
     リセット理由、fw バージョン (git describe)、電池電圧 mV、vbus、BLE の RSSI と接続間隔、
     現在の画面。電話に「デバッグ」画面を追加し、5 秒ごとに更新する (画面を開いている間だけ)。
  2. 設定の汎用エディタ: `settings.get` の全キーを型に応じて (u32/i32/str) 編集できる画面。
     既知のキーは日本語ラベルと選択肢付き、未知のキーは生の値を編集する。
- 受け入れ条件: protocol-v1.md に追記。core のテスト (debug.info の応答形) と電話のテスト。

状態: 完了 b64d563

## T14. 直感的でない操作の修正 (A) — 難度 M

- 操作ルール (これを正とする):
  - PWR 短押し = 戻る (Home では画面 OFF)。Boot 短押し = その画面の主操作。
    PWR 長押し = 電源メニュー。
  - 右スワイプ = 戻る (Home 以外の全画面。横スクロールする部品の上は除く)。
  - リストは縦スクロールだけ。確定は行のタップ。破壊的な操作は確認ダイアログを経る。
  - 「戻る」ピルとハードの PWR は常に同じ動作をする。
- 全画面をこのルールで監査し (コードと sim で)、違反を一覧にしてから直す。
  監査結果は handoff.md に残す (現在は docs/archive/handoff-2026-10-15.md の T14 節)。

状態: 完了 7837edd

## T15. テーマを GUI で作るツール (H) — 難度 M

- 決定: `tools/theme-studio/index.html` をビルド不要の単一 HTML (素の JS) で作る。
  - 左側: tokens の色ピッカーと数値、images/screens.bg/icons のファイル選択。
  - 右側: 410x502・角 R100 のマスク付きプレビュー。Home、リスト画面、ボタン群、
    文字盤の 4 つの HTML 再現で、色を即時に反映する。
  - 書き出し: manifest.cbor (小さな CBOR エンコーダを同梱) と、STORED zip
    (CRC32 は自前実装) をダウンロードする。
  - 既存の zip を読み込んで編集を再開できる。
- 受け入れ条件: 書き出した zip を core のマニフェスト/zip パーサのホストテストで読める
  (テスト用のサンプルを `core/tests/data/` に置く)。sim の `--theme` で表示できる。

状態: 完了 3356f7a

---

# 第2弾 (2026-10-13 ユーザーフィードバック由来)

ユーザーからの確認結果: T9/T11/T12 は実機で OK。数字の色は今のままでよい。
T14 の「要判断」4件は変更しない。行の補足テキストを -14px 重ねて詰めている件は、前任 AI の判断なので変えてよい。
**T6 は下の T16 に吸収する** (数字の色は変えない。残りの確実な違反は T16 と一緒に直す)。

## T16. 文字の重なり・豆腐の一掃 (最優先) — 難度 M

- 症状 (ユーザー報告): 「文字が色んなところで被りすぎ」「`-` がフォントに無く豆腐」。
  T4 のフォント刷新 (行 26px・見出し 32px・行間 1.25) で高さが増えたのが主因と推定。
- sim 監査に2つ追加する (どちらも 0 件を受け入れ条件にして CI で fail させる):
  1. **テキストの重なり**: 同じ画面で可視のラベルどうしの bbox が交差したら報告。
     親子・同一オブジェクトは除外する。装飾は対象外。
  2. **欠けグリフ**: 各ラベルのテキストを `lv_font_get_glyph_dsc` で検査し、
     フォントに無いコードポイントを報告 (フォールバックフォントも辿る)。
- 修正方針:
  - 全フォントに ASCII 0x20–0x7E を必ず含める。jp_32 の subset にも入れる。
    `‐ – — ・ … 「」` など UI でよく使う記号も含める。
  - 重なりは行・カードの高さを内容依存 (SIZE_CONTENT + min_height) にして直す。
    ぎりぎり固定の高さはやめる。-14px の重ね詰めはやめて、行間で調整する。
  - T6 監査 (docs/archive/t6-audit.md) の「確実な違反」のうち、数字の色に関するもの以外も直す。
- 受け入れ条件: 全テーマ × 全画面で、重なり 0・欠けグリフ 0・safe_area 0。
  変更前後のスクショを並べてユーザーに見せる。

状態: 完了 68d0eac (+b545625 スクショ)

## T17. 電源・入力の仕様変更 — 難度 M

1. **PWR 短押し = どの画面でも画面 ON/OFF のトグル** (戻るにしない)。
   戻るは戻るピルと右スワイプで行う。
   InputMapper の既定を `button.pwr.short = "screen_off"` にする (消灯中は既存の仕組みで Wake)。
   電話側の既定値、protocol-v1.md、plan.md G章、テストも合わせる。
2. **タイマー終了・アラーム鳴動で自動的に画面 ON**: core の Runtime で
   TimerFinished/AlarmRinging の Event を受けたら、power を Wake (kick_activity + Active)。
   消灯中でも alert が見えるようにする。
3. **アプリから「画面を消さない」を指定できる**: FeatureDescriptor に
   `bool (*keep_screen_on)(const FeatureContext&)` (nullptr 可) を追加する。
   Runtime は「現在の Route の Feature」の keep_screen_on が true の間、
   Dim/ScreenOff への自動遷移を止める。手動の PWR 消灯は効く。
   画面を離れたら効果は切れる (バッテリー保護のため)。
   stopwatch は計測中だけ true にする。timer にも同様に入れる。
   core テスト: 該当画面の間は遷移しないこと、離れたら遷移すること、手動消灯は効くこと。
4. **画面の長押しを電源メニューに使わない**: 画面長押しのグローバルな割り当て
   (powermenu 等) を外す。新しい ActionType `LongPress` を「現在の Feature の handle」に渡す
   (既定は何もしない)。電源メニューへは PWR 長押し (従来どおり) で行く。

状態: 完了 4ba3a2b

## T18. 録音できない (T10 の続き) — 難度 M (実機)

- 症状: メモの録音開始を押しても反応しない。メモが増えない。
- 手順: 録音のボタン → Action → memo feature → AudioPort::record_start → mic_open → I2S read →
  ファイル保存、の各段に ESP_LOGI を入れる。書き込んだあと、モニタをバックグラウンドで
  10分ほどファイルに取り続け、ユーザーに「メモ → 録音開始 → 5秒 → 停止」を頼んで、ログで
  どの段が止まっているかを確定する。
  疑わしい点: ボタンの CLICKED が Action になっていない、can_record の分岐、
  録音タスクのスタック/SRAM、/storage の littlefs が未マウント (audio init 内でマウントしていた →
  MIC だけ有効のときに通っているか)。
- 直したら診断ログは消す。

調査の経過 (2026-10-14、/tmp/t18_rec.log + /tmp/t18_rec2.log):

- **第1の原因 (確定・修正済み `5680d23`)**: `mic_open` が esp_codec_dev の I2C ctrl に
  `.addr = 0x40` (7bit) を渡していた。codec_dev は 8bit 表記を要求するので
  `0x40>>1 = 0x20` に書きに行き ES7210 への全書き込みが NACK に
  (`I2C_If: Fail to write to dev 40` → `ES7210: Open fail`)。
  `ES7210_CODEC_DEFAULT_ADDR` (0x80) に修正済み。
- **副次的バグ (確定・修正済み `5680d23`)**: `mic_open` が `i2s_new_channel` 成功後に
  失敗すると I2S チャネルと data_if/ctrl_if がリークし、以降の試行が
  「i2s controller 1 has been occupied」で永久に失敗していた。
  全失敗パスで `i2s_channel_disable` → `i2s_del_channel` → `audio_codec_delete_*` を
  呼ぶ巻き戻しを入れた。
- **現在の停止箇所 (未修正)**: 修正後のログはボタン → rec toggle → mic_open →
  `i2s_new_channel` 成功 → `i2s_channel_init_std_mode` で
  `i2s_alloc_dma_desc: allocate DMA buffer failed` → `mic i2s init/enable failed` →
  `record_begin refused`。**DMA 可能な内蔵 RAM の連続ブロックが録音時には残っていない**
  (この個体は BLE_INIT も直前に `Malloc failed` を出した履歴がある。内蔵 SRAM が
  逼迫している)。
  - 次に疑う/試す箇所: `chan.dma_desc_num=4, dma_frame_num=240` をさらに小さくする、
    もしくは I2S チャネルを録音要求時ではなく `audio::init` で確保しておく
    (起動時の方がヒープが残っている)。DMA 領域は PSRAM に逃がせないので
    内蔵 RAM の予約しかない。
解決 (2026-10-20、T24/T29 で改善したメモリ状況で再挑戦):

- **DMA 確保の前倒し (確定)**: `audio::init` で I2S RX チャネル + DMA
  (desc=4 × frame=240) を確保しておき、`mic_open` では `i2s_channel_enable`
  するだけにした。起動直後は内蔵 RAM が残っているので確実に取れる。
  録音終了時はチャネルを削除せず disable のみ。
- **`esp_codec_dev_read` の戻り値の誤解釈 (確定)**: この API はバイト数ではなく
  ステータス (`ESP_CODEC_DEV_OK == 0`) を返す。`rd <= 0` で捨てていたため
  常に 0 サンプルだった。PCM バッファを事前ゼロクリアし、成功時は全フレームを
  処理するよう修正。
- 実機検証 (シリアル `v` コマンド): `mic_open ok` → ES7210 unmute →
  **samples=88064 (~5.5s @16kHz)、ADPCM 44,048B**。診断ログは除去済み。
- 補足の `id=0x80000000` はシリアル `v` コマンドが直接 `record_begin` に渡す
  テスト用 id であり、memo id カウンタの異常ではない (確認済み)。

状態: 完了 0269bb4 (シリアル `v` で録音〜ADPCM 保存まで実機確認。
2026-10-22 にユーザーがメモ画面から録音・保存できることを確認。
スマホへの音声メモ転送は別件 → T30)

## T19. アプリを本体改変なしで追加できる仕組み + 開発ガイド — 難度 M

- 現状: `apps/<id>/` があるが、`enabled.txt`、`SAS_APP_*` の `#if`、
  `core/features/registry.cpp` の `kBuiltin[]`、applist の行などを手で編集する必要がある。
- 方針: **CMake で apps を自動発見して、登録表を生成する**。
  - 各アプリに `apps/<id>/app.cmake` を置く。中身は id・表示名・アイコン (font_icon の
    コードポイントまたはテーマ画像 id)・ソース一覧・有効/無効。
  - CMake が `apps/*/app.cmake` と、外部ディレクトリ `WATCH_EXTRA_APP_DIRS` (; 区切り) を走査し、
    `generated/app_registry.cpp` を生成する。中身は Feature の表・画面生成関数の表・applist の表。
  - core / firmware / sim の 3 つのビルドが同じ生成物を使う。`kBuiltin[]` と
    applist のハードコードはこれに置き換える。
  - 既存アプリを全部この形式に移す (挙動は変えない)。
- `apps/_template/` (最小アプリ: カウンタ相当) と `docs/app-guide.md` を作る。
  ガイドには、Feature の書き方、画面の書き方 (ui::c 部品)、keep_screen_on・LongPress、
  設定・保存 (kv キーは 15 文字以内)、テスト、外部リポジトリからの追加方法を書く。
- 受け入れ条件: `apps/_template` をコピーして名前を変えるだけで、applist に出て動く
  (sim スクショ)。外部ディレクトリのアプリも CI で 1 つビルドする。

状態: 完了 e7d11b7

## T20. テーマ画像を id で自由に増やせる — 難度 M

- 現状: images 4 枠・icons 12 件が固定長。
- 方針: manifest v5 の `assets`
  (上限は zip のエントリ数 32 に従う。id は `[a-z0-9_.-]{1,31}`)。
  - 既存の images/icons キーは assets の別名として読む (互換を保つ)。
  - アプリやテーマ設定からは `theme_image("<id>")` で引く。
    applist のアイコンは `icon.<app_id>` を引く。T19 のアプリも同じ仕組みを使う。
  - 遅延ロードにして、使われる画面でだけ読む (グローバル常駐はスキン・アイコンのみ)。
- テーマスタジオ: 画像の枠を「+ 追加」で増やし、id を入力できるようにする。
  アプリ一覧のアイコン枠は、T19 の生成した app 一覧から作る。
- docs/theme-format.md を更新する。

状態: 完了 8a1d41c

## T21. テーマスタジオの拡張 (kit.html とフォント) — 難度 M

- 既存の HTML 機能: `themes/<id>/kit.html` を `tools/build_themes.py --kit` が
  Playwright でスクショして 9-slice スキンを生成する (theme-format.md v3)。
  スタジオからは今は触れない。
- 方針:
  - スタジオに「スキン」タブを作る。kit.html を読み込んで iframe でプレビューし、
    CSS 変数 (色・角丸・影) をスタジオの tokens と連動させる。
  - 書き出しの選択肢を2つ用意する。
    (a) 編集済みの kit.html を保存し、`build_themes.py --kit` で zip 化する (高品質・推奨)。
    (b) ブラウザだけで、SVG foreignObject → canvas → PNG で各 `data-part` を書き出す
        (外部フォントが無ければ十分な品質)。
  - **フォントはテーマで指定できる** (`fonts` キー: digits/digits_sm/title/body、
    lv_binfont 形式)。スタジオでも .bin フォントを指定できるようにする。
    TTF からの変換コマンド `tools/gen_fonts.py --binfont <ttf> --size N` をスタジオ内に表示する。
    無ければ作る。

状態: 完了 4272a42

## T22. Home と文字盤の整理 — 難度 H (調査 → ユーザー確認)

- ユーザーの疑問: 「Home と文字盤は何が違うのか」。
- まず実装を調べる: Home Route の中身、face の選択、home_bg/stand 画像、face_layout の関係。
  そのうえで「Home = 選択中の文字盤の表示」に一本化する案を出す。
  home_bg/stand は standard 文字盤の素材として assets 化する。
- 案をユーザーに見せてから実装する。

状態: 完了 (提案のみ) 09d43fa

## T23. 画像付きテーマで起動時にクラッシュループ (最優先の製品バグ) — 難度 H (実機)

- 事実 (2026-10-13 実機): theme=mame で起動すると、毎回 1.6s で LoadProhibited になり、
  1112 回リセットを繰り返した。バックトレースは
  `ui::create → theme_apply → img_load("home_bg.bin") → fread → littlefs → esp_flash_read →
  get_buffer_malloc` で、TLSF の free-list 走査中に壊れたメタデータを踏む。
  16KB ずつ分割して読むと、2 chunk 目までは成功し 3 chunk 目で落ちる。
  batch2 世代の古いバイナリでも同じなので、潜在バグ。
  main のスタックを増やしても変わらない (スタック溢れは否定)。
  NVS を消去して standard テーマに戻すと正常に起動する
  (このためユーザーのテーマ・文字盤・時差の設定が消えた)。
- 方針: 落ちる箇所ではなく、**それより前に heap を壊している側**を探す。
  - `heap_caps_check_integrity_all(true)` を theme_apply の各段の前後と、fread の各 chunk の前後に入れる。
    最初に壊れたことが分かる区間を二分探索で絞る。
  - 第一容疑: theme_res のアリーナ / 小アロケータ (T8 の deco 共有ブロック、T20 の遅延ロード)
    と、RGB565A8 のサイズ計算の取り違え (w·h·2 と w·h·3) による範囲外書き込み。
    `.bin` ヘッダの stride と実データ長の照合も見る。
  - 防御として、テーマ適用で例外的な失敗 (クラッシュ) が起きたら、次回起動では standard で
    起動する仕組みを入れる (RTC メモリか NVS に「適用中」フラグを書き、正常完了で消す)。
    クラッシュループで設定を消す必要を無くすため。
- 受け入れ条件: mame/cosmos/cute/cyber のどれを設定しても、再起動を 3 回繰り返して正常に起動する。
- 診断ビルドは `/tmp/t18_elf` (85bedc2 + 計装 + heap poisoning) にあった。/tmp は消えうるので、必要なら作り直す。
- 静的解析 + ASan/UBSan sim (mame.zip 実物) では自前コードの範囲外アクセスを発見できず。
  fread(dst=PSRAM) → esp_flash_read が内部 RAM の一時バッファを経由する経路
  (esp_flash_api.c direct_read=false) がクラッシュ面。対策として
  `theme_entry_read` を DRAM 16KB ステージングに変更し、ui::create 中の
  クラッシュを RTC ガードで検出して次回 standard フォールバックする。
- **真因 (2026-10-14 確定)**: `core/theme/manifest.cpp` の
  `theme_manifest_parse` が `ThemeManifest o;` (~11.7KB) をスタックに
  取っていた。LVGL タスク (7168B)・main (3584B)・NimBLE ホストの
  どれでも overflow → 隣接メモリ破壊が IntWDT/TLSF assert/
  LoadProhibited など場所不定の panic として出ていた。
  診断 ckpt で `taskLVGL hwm=104` (manifest 段) を直接観測。
  修正 033a704: `new (out) ThemeManifest()` で out 直書き。
  併せて main タスクを 8192B に拡大 (997bb28)。
  実機検証: mame のランタイム適用 + 再起動 3 回で panic/fallback なし。
- 残置: RTC ガードの standard フォールバック、DRAM staged_fread、
  `theme: apply` / `pkg:` の起動ログ。

状態: 完了 033a704 (+997bb28)

## T24. flash の app 領域が残り 8% — 難度 M

- T16 のフォント補完で 0x2E→0x3A (+~770KB) 増えた。
- 方針: 日本語フォント (jp_20/26/32) を app バイナリから外し、assets パーティションの
  binfont (`lv_binfont_create`、PSRAM にロード) にする。起動時に 1 回読む。
  失敗したら最小の内蔵フォント (ASCII+かな) にフォールバックする。
  あわせて、パーティション表で app を拡張できる余地も確認する (32MB flash)。OTA の 2 面構成を保つこと。
- 受け入れ条件: app 領域の空きを 30% 以上に戻す。sim の監査 0 件を維持する。
- 結果 (実機): app 空き 8% → 31%。jp_20/26/32 を `tools/fonts/bin/*.bin`
  (lv_binfont、計 ~1.4MB) として assets に移し、`ui::init_display` で
  PSRAM にプリロード。ローダはグリフ表を LVGL ヒープに複製するので
  ソースバッファは create 後に即解放、残量ガード付き。バイナリ内には
  ASCII+かな+記号の最小フォント (font_jp_min_20) だけ残し、
  binfont が読めない/足りない時はそちらにフォールバック。
- PSRAM の分断対策: テーマアリーナを「資産領域」と「画面ブロック
  (640KiB×2)」に分割し別個に確保するようにした (単一 ~3MB 連続確保は
  フォント常駐後は失敗し、mame の資産が欠けていた)。
  資産がそれでも入らない時は最大連続ブロックに縮めて開く。
- 残り: 肌テーマ (cute/cyber) でアプリ一覧の bake (~850KB) は連続空き
  ~500KB を超えられずフォールバックになりやすい (見た目は同じで
  スクロールが ~180ms と遅い)。bake 確保失敗時はスキンキャッシュを
  捨てて一度だけ再試行する。パーティション表の拡張余地は未確認。

状態: 完了 92a5cb8

## T25. BLE テーマ転送 (BULK) の安定化 — 難度 H (実機)

- 経緯 (2026-10-14): mame.zip (918KB) の BLE 転送が連続して失敗したため先送り。
  4 テーマは assets イメージ (`tools/mklfs_assets/make_assets_image.sh`) で
  最初から時計に入れる運用に切り替えた。BULK 経路自体は残す。
- 確定した事実:
  a. NimBLE ホストタスク上での `theme_store::bulk_write` (littlefs fwrite =
     フラッシュ書き込み+キャッシュ停止) が HCI 処理を滞らせ、受信 mbuf が枯渇 →
     `assert failed: ble_att_tx_with_conn ble_att_cmd.c:91` で再起動する
     (「画面が一瞬真っ白」の正体)。診断ログ (bulk START/END/rejected) は
     ble_link に残してある。
  b. PSRAM は転送開始時点で空きが ~38KB しかない (テーマアリーナ 4MiB +
     LVGL 等)。918KB の staging を PSRAM に置く方針は破綻する
     (01adcd4/f766dcd は revert 済み)。
  c. bulk_rx ワーカータスク方式 (4a47742) は 6KB スタックで overflow
     (fwrite→littlefs→install の深い呼び出し)。10KB 化 (7bce96e) 後も
     ユーザー報告で接続直後に応答停止 → status=8 切断・白画面になった
     (モニタ欠損で panic 行は未捕獲 — ハングかログ喪失かは未確定)。
     この2つも revert 済み。
     注: theme_store::install の検証 parse は static だったため
     ThemeManifest のスタック問題 (T23 真因) とは別因の可能性がある。
  d. 転送中の queue full drop は ACK 巻き戻しで自己回復するが、
     fwrite (~200B/chunk) が流入に追いつかないと大量の再送が起きる。
- 次の方針候補:
  - ワーカータスクのスタックを PSRAM に置く (xTaskCreateStaticPinnedToCore
    に PSRAM バッファ — S3 ではスタックの PSRAM 配置は不可。内蔵必須 →
    10KB+ を ble_link_start で早期確保する形は既に試した)
  - 電話側の送信レートを抑える (N チャンク毎に ACK 待ちするよう
    BulkSender を変更 — window 化)
  - 書き込みをチャンク単位ではなくファイルへまとめて flush
    (littlefs の 4KB block 単位で書き溜める)
  - END 応答を commit 完了まで遅らせず ack → 別タスクで install
    (phone 側 BULK_OP_TIMEOUT_MS=10s との兼ね合い)

状態: 先送り

## T26. テーマ適用時の PSRAM 枯渇 → 内蔵 RAM 崩壊で abort — 難度 M (修正済み)

- 症状: cute (PNG スキン系テーマ) を適用後、アプリ一覧を開くと
  `lv_realloc: couldn't reallocate memory` → `lv_draw_buf_create_ex:
  No memory` (366x61/300x65/346x611=422KB のレイヤー) → `BLE_INIT:
  Malloc failed` → abort。再起動しても同じループで `int_largest=20`。
- デコードした backtrace: abort → newlib `lock_init_generic` (locks.c:77)
  ← `__sfp` ← `fopen` ← `zip_open_list` ← `theme_entry_stat` ←
  `decode_png` ← `img_decode` ← `skin_img` ← `skin_base`。
  画面生成時のスキン PNG デコードが fopen し、その FILE のロック確保
  (内蔵 malloc) が失敗して abort。
- 原因: テーマアリーナを PSRAM に**常時 4MiB**確保していたため
  残りが ~38KB しかなく、LVGL のレイヤーバッファ・lodepng 一時領域・
  FILE 構造体が内蔵 RAM (空き ~130KB) に集中して枯渇。
- 修正 (393ed97): アリーナを**manifest 見積もり分だけ**確保し直す方式に。
  `theme_res::assets_estimate` が画像スロット/icons/mascot/face/skin/
  font/assets の decode 後サイズを計算 (.bin=ファイルサイズ、.png=IHDR
  の w*h*3)。アリーナ = 画面ブロック 640KiB×2 + 見積もり (上限 3MiB)。
  manifest はアリーナ外の tmp に読んでから parse。
  内蔵テーマは資産不要なのでアリーナは 1.25MiB のみ。
- 実機検証 (393ed97 書き込み後): cyber で `apply -> cyber`・Home 35ms・
  `ble synced=1`・`int_largest=8704`・lv_realloc/No memory ゼロ、
  リセット再起動でも再発なし。単発の `BLE_INIT: Malloc failed` が
  apply 直後に 1 回出るが回復 (観察継続)。
- 補足: LVGL の malloc は IDF 既定 (内部優先→PSRAM フォールバック)。
  PSRAM に余裕ができたので大きな確保は自然に PSRAM に行く。
  LVGL プールを PSRAM 優先に変える変更は現時点では不要と判断。
- 残件: `theme_tmp_alloc` の lodepng 一時領域 (大画面 PNG で ~800KB)
  も PSRAM を使う — 見積もり済みでも極端に大きいテーマは要観察。
  T24 (app 領域 8%)・T25 (BULK) と合わせてメモリ予算の見直し余地。

状態: 完了 393ed97

## T27. 実機 UI の描画不具合 (ユーザー報告まとめ) — 難度 S〜M

実機でユーザーが見つけた4件。個別に切り分けてよい。

- a. **メニューの枠の角が黒く塗りつぶされる** (角丸の外側が黒い)。
  候補: パネル物理角R ≈100px (handoff.md の実機確定値) に対する描画領域のクリップ
  不足、`skin_base` の scrim/bg が角まで覆う、`clip_corner` 未指定の
  親コンテナ (shell.cpp / components.cpp の card/group)。
- b. **スクロールの表示領域が途中で切れて下まで表示されない**。
  候補: リストの高さ計算 (410x502 固定?) と下部セーフエリア/ナビゲーション
  バー分の考慮漏れ。screens/*.cpp の `lv_obj_set_height`/flex の行。
- c. **ストップウォッチの一番下に謎のテクスチャが出る**。
  候補: `skin_base`/`skin_front` の deco の誤配置、画面ブロックの
  再利用で前テーマの残骸が残る (theme_screen_block)、またはクリア
  されていない描画領域。実機の screen=stopwatch で再現確認。
- d. **スライダーのつまみの丸が見切れる** (cute のみ)。
  候補: スキンの switch/slider_knob 画像が LVGL の knob 領域より大きい。
  track の pad / `ext_draw_size` / skin slice の調整。components.cpp:359
  の slider 描画で knob 領域と画像サイズの不整合を確認。

受け入れ条件: 各項目、実機または sim snapshot で再現→修正→消えたことを
確認 (a/b/d は sim の parts 画面でも見られる可能性あり)。

調査・修正メモ (全件 sim snapshot で再現→修正→消滅を確認):
- a. 原因は `bake_scroll` の `lv_snapshot_take` が RGB565 (アルファ無し)
  で、top_obj あり時はバッファ未クリア → カード角の透過部が未初期化
  メモリ (実質黒) として焼き付いていた。→ ARGB8888 + 明示クリアに変更
  (components.cpp)。
- b. `content()` が下端を y=464 (502-76-38) で打ち止めていた →
  下端までスクロール領域を延ばし、`pad_bottom=38` で末尾要素を安全域に
  収める形に変更。sim 監査はスクロールコンテナ内の途中位置を角判定から
  除外するよう調整 (snapshot.cpp)。
- c. `stopwatch` の空 `laps_col` グループが 0 ラップ時にカード枠だけの
  細い帯として描かれていた → lap_count==0 で LV_OBJ_FLAG_HIDDEN
  (apps/stopwatch/screen.cpp)。
- d. `slider_knob.png` が kit.html の `--m` (キャプチャ余白) 未指定で
  外側 3px リングが撮影範囲外に切れた不良アセットだった → shot 30x30 +
  --m:3px に修正し再生成 (themes/cute/kit.html → build_themes.py --kit)。
  あわせてスライダーのノブ描画領域を 24→30px に (kSliderKnob)。
- ついでに powermenu の警告文が重なっていた件も修正
  (`line_space=-12` → -4。font_jp_20 の実行高は 24px)。

関連修正 (ユーザー追加報告「deep sleep から復帰できない」):
- 入眠時の `pmic::panel_power(false)` が AXP2101 レジスタに保持されたまま
  復帰後もパネル無電源だった → `pmic::init()` で `panel_power(true)`。
- BOOT(GPIO0) の ext0 起床は strapping pin のため押下起床が download
  mode に落ちる → ext0 を廃止 (PWR/IMU/timer のみ)。docs/board.md,
  docs/plan.md も更新。
- `arm_wake_on_motion` 末尾で `wom_clear()` し、未クリア WoM ラッチによる
  入眠直後の即起床ループを防止。
- raise_to_wake OFF 時は ext1 マスクから GPIO21 を除外
  (`enter_deep_sleep(us, imu_wake)` にシグネチャ変更)。
- IMU 起床が効かない根本原因: vendored `qmi8658_init` が `CTRL1=0x60`
  (INT1_EN=0) で INT1 ピンを一度も駆動していなかった — STATUS1 にだけ
  WoM が記録され ext0/ext1 は構造的に発火不能。`arm_wake_on_motion` で
  `CTRL1=0x68` にして解決 (実機で `wake reason: ext0 (IMU)` 確認)。
- あわせて実機検証で潰したバグ: 入眠窓で imu_task が WoM arm を再呼出し
  INT1 極性を上書き → `imu_service_freeze_for_deep_sleep()` で凍結。
  プローブの `rtc_gpio_deinit` が ext0/ext1 のパッド設定と LP IO クロックを
  落とす → deinit は GPIO0 のみに。I2C バスロック (リセット乱用でスレーブが
  SDA 保持) へのブート時 SCL トグルリカバリも追加。
- デバッグ経路: USB-Serial/JTAG の 's' 入力で強制 deep sleep
  (serial_dbg_task)。自動深眠は `deep_sleep_after_s` (既定 1800s) で動作。

追加 (raise_to_wake の電池対策 — IMU 起床ゲート):
- WoM は「任意の動き」で発火するため (現在は 224mg + blanking 12 サンプル)、
  深眠中に腕が動くたび
  起きて画面ON → 30分 ScreenOff アイドル (BLE 生きてる) を繰り返す
  と電池を食う。そこで ext0/ext1(GPIO21) 起床時は app_main で
  表示・BLE・LVGL を立てる前に IMU で姿勢を計測し、入眠時に保存した
  基準姿勢 (重力方向、RTC RAM 保持) からの角度差と絶対姿勢の
  両方で腕上げを判定し、合わなければ即再入眠する (数100ms〜1.5s
  で寝直せる)。PWR 起床とタイマー(アラーム)起床は素通し。
  USB 接続中は寝直さない (デバッグ用)。基準姿勢は
  `board::sleep::set_wake_reference()` で WoM arm 前に採取
  (arm 中は出力レジスタが凍結するため)。
- 判定値は RTC RAM の `last_gate_debug()` で起動後に確認できる
  (USB ポート開放前のゲートログは喪失しうるため)。シリアル 'g' で
  いつでも再表示可能 (n/deg/dir/pass + entry 診断付き)。
- 採用条件 (装着したままの 12 姿勢計測で決定 — 後述):
  `静止3連続 && deg>=30 && dir.z<=-0.65 && dir.x>=-0.35`。
  角度だけだと前伸ばし 60°/腕組み 73°/打鍵 75°/スマホ持ち 72° が
  「見る」動作と区別できないため、画面法線 z (上向き) と手首回転 x
  の絶対方向で絞る。方向条件は core RaiseDetector::Config の
  face_check と同値 (screen off のソフト検出でも同じ判定)。
- 実機検証で潰したバグ (起床ゲート編):
  - WoM arm は ESP リセットを跨いで持続し、arm 中は出力が凍結する
    → 起床ゲート内で `disarm_wake_on_motion()` してから計測。
  - WoM 発火→起床→判定までの間にジェスチャがまだ動作途中 →
    最大 ~1.2s サンプリングして「直近と 10° 以内が3連続」の静止姿勢
    で判定。安定未達で打ち切った場合は閾値超過でも採用しない。
  - 入眠直前の INT1 LOW 待ちは `disarm→読取→arm` を反復すると
    arm 直後のピン遷移ラグで毎回 HIGH を読み攪拌する → arm は
    触らず STATUS1 読取だけで LOW 連続3回を待つ方式に (実測で
    'c' 単発は LOW に落ちることを確認)。
  - deep sleep のタイマー期限に `s_rt.next_deadline_ms()` を使うと
    PowerPolicy の画面遷移時刻 (Dim 8s 等) まで min に入って数秒で
    タイマー起床 → feature の期限だけ (`s_features.next_deadline_ms()`)
    を使うよう修正。
  - WoM 閾値 64mg (plan.md 例値) では装着中の微振動で常時発火し
    入眠/起床を往復 → 192mg + blanking 8サンプル (~380ms) に変更。
    2026-10-24: 机の振動で誤起床 (`deg=2.3 / pass=0`) したため、224mg +
    blanking 12 サンプル (~570ms) に引き上げ。装着して 1 日の再確認が必要。
  - PWR 押下の動き自体が WoM を先に発火させ ext0 起床になることが
    ある → ext0 起床でも ext1 status を読み、PWR ビットがあれば
    ゲートをバイパス (ボタンは常に起きる)。`n=-2` で記録。

実機検証結果 (2026-10-18, 装着したまま):
- 's' で入眠 → 静止では 22s 以上眠り続ける (still_iters=2, INT1=0
  で綺麗に入眠。タイマー期限も正常値 ~8000s)。
- 見る持ち上げ: `deg=60.2 dir=0.5,0,-0.8` / `deg=62.8 dir=0.1,-0.3,-0.9`
  → **pass=1 採用**で画面ON。
- 見ない動き (前伸ばし/横向き等): `deg=72.7`, `deg=40.2`, `deg=20.0`
  → **pass=0 拒否** (角度が大きくても方向条件で弾く)。
- 姿勢の生値計測 (12 姿勢): 見る系は z<=-0.71/x>=-0.13、
  非対象は x<=-0.41 (前伸ばし/横/顔近く/スマホ/打鍵/ポケット/腕組み)。
- PWR 押下・タイマー起床はゲート素通しで正常起動を確認。

残件: a のパネル物理角・d の見え + 装着したままの実生活での
誤作動率 (face 条件は左手首・計測値ベースのため要継続観察。
装着位置/左右手が変われば face_z_max/face_x_min の再計測が必要)。

状態: 完了 (deep sleep 復帰 + 起床ゲート + 装着キャリブレーション済み)

## T28. 時計の設定画面からファイルテーマを選べるようにする — 難度 S

- 現状: 時計側のテーマ選択は「標準 (ダーク)」と「明るい」の2択のみ。
  file テーマ (assets の zip) は電話からしか切り替えられない
  (settings.cpp:215-237 のコメント参照)。
- やること: 時計の設定画面のテーマ行に `/assets/themes/*.zip` を
  readdir で列挙して追加し、`settings.theme` に書き込んで適用する。
- 受け入れ条件: 電話なしで 4 テーマを時計側から切替・再起動で持続。

実装 (2026-10-22):
- `ui::theme_list_ids()` (`theme_manager.cpp`) が `port::theme_assets_root()` 直下の
  `<id>.zip` を readdir で列挙する (`theme_id_ok` のみ・重複除去・昇順・最大 12 件)。
- 設定画面のテーマ節に内蔵 2 種 + zip テーマを並べ、タップで `SetTheme`
  (settings.theme に保存 → shell が全画面を作り直して適用)。適用中の zip が
  assets から消えている場合も「使用中」行は残す。
- sim: テーマ節を `12_settings_theme` として撮る (`sim/themes` の zip が出る)。
  安全域 / 重なり / 欠けグリフの監査は 0 件。
- 検証: sim で `cute` 行をタップ → `settings.theme=cute` / テーマ適用を確認
  (設定画面が cute スキンで再構築され「キュート 使用中」になる)。
  実機は書き込み後に起動と設定画面の生成 (bench 90ms) まで確認済み。

状態: 実装済み・実機確認待ち (未コミット)

## T29. 時計の動作が重い (ユーザー報告 2026-10-19) — 難度 M (実機)

- 計測手段を先に作った: シリアル `b` で主要 6 画面の全面再描画とスクロールの ms を出す
  (`ui::perf_bench`、docs/perf.md)。テーマ切替は `t`。
- 原因と対処 (詳細と数値は docs/perf.md):
  1. 描画バッファが内蔵 SRAM の 10 行 ×2 で、1 フレームが ~50 パスに割れていた →
     PSRAM の全面バッファ 1 枚 + 内蔵 8.2KB バウンス 2 枚で転送 (`esp_display.cpp`)。
     standard で全面 85〜112ms → 40〜68ms。内蔵 SRAM は起動時に +8KB。
  2. 9-slice スキンの伸縮描画が毎フレーム LVGL のソフト変換を通っていた →
     (画像, サイズ) ごとの合成結果をキャッシュ (512KB)。cute/cyber の設定・タイマーで 2 割減。
  3. 副作用: 全面 PNG を lodepng で展開すると ~1.65MB 要り、cosmos の背景が消えた →
     行ストリーミングの PNG デコーダ (`png_stream.cpp`, ROM tinfl, 作業 ~50KB)。
     不透明な画像は RGB565 で持つ (毎フレームの α ブレンドが不要)。
- 実機ベンチで見つけて直した潜在バグ:
  - spi_bus_lock のマルチコア競合で UI が凍る (IDFGH-17166) → LVGL タスクと SPI ISR を core1 固定。
  - 旧画面の削除タイマーが遷移アニメ完了より先に走り、`prev_scr` が解放済みを指して落ちる → 延期。
  - shell のイベントリングが ClockTick で埋まりうる → ClockTick は未処理 1 件に畳む。
- `managed_components/` に置かれていた性能パッチ (SPI 割り込みレベル、タッチ読取タスク、QSPI 80MHz) を
  `firmware/vendor/` に移して追跡するようにした (CI と clean clone では消えていた)。
- 続き (2026-10-20): 大きい部品 (>=2048px) はソースを 1D 走査して単色/グラデ/縞を
  矩形塗りで近似する fill/band パスを追加し、cute 設定 195→141ms、cyber 設定 224→157ms、
  cyber タイマー 219→144ms。縁ストリップは伸縮補間が見た目の一部なので画像のまま。
  LVGL ヒープを PSRAM 化し (lv_mem_psram.c)、表示中の内蔵 SRAM 空きが ~0.5KB → ~18-21KB。
  touch の NACK (FT3168 スリープ中のセーフティポーリング) と `swap_xy` 警告を vendor パッチで解消。
- 残り: 縁ストリップとスライダー塗りは画像描画のまま。bake の PSRAM 断片化も残る。

状態: 完了

## T30. 音声メモをスマホへ送れない (ユーザー報告 2026-10-22) — 難度 M (実機)

- 症状: 時計で録音・保存はできる (2026-10-22 にメモ画面から録音を実機確認)。
  スマホへ送る操作でエラーになり、音声メモを取得できない。エラー文言と
  時計/電話どちらのログも未採取。
- ユーザー判断: 音声メモはほぼ使わないので後回し (2026-10-22)。
- 再開時に見る場所: 電話 `MemoScreen` / `WatchViewModel` → `memo.list` / `memo.get`、
  時計側の `memo.*` 実装 (docs/protocol-v1.md の memo 表) と `memo.audio.get` 後の
  BULK (kind `memo`)。まず電話側のエラーダイアログ/ログを取って切り分ける。
- 受け入れ条件: 録音した音声メモが電話の一覧に出て再生できる。

再開 (2026-10-23、T31 の文字起こし実機テストで再現):
- 実機ログ: `bulk out start kind=memo file=2 size=29968` → 22ms 後に
  `bulk out id=2 stopped: send failed`。STT まで到達していなかった。
- 原因: 時計→電話 BULK の送信バッファ枯渇。NimBLE の mbuf は
  MSYS_1 256B × 12 個。512B チャンクを 8 個まとめて送ると 244B 通知 ×24
  必要になり、13 個目で `ble_hs_mbuf_from_flat` が失敗していた。
- 修正 (2026-10-23、効果なし): `notify_emit` で `BLE_GAP_EVENT_NOTIFY_TX` を
  待つ再試行を入れたが、このイベントは**送信試行時に同期的に**発火するため
  待ち合わせになっていなかった。
- 修正 (2026-10-24): チャンク長を MTU から算出 (`ble_link_bulk_chunk_max`、
  MTU 247 → 232B) し、1 チャンク = 1 通知に収めた。8 チャンクのウィンドウ
  でも mbuf は 8 個で済む。加えて BULK のみ、確保失敗時に 5ms 待って
  最大 20 回再試行する。
- 実機 (2026-10-24): 30KB / 13s で `bulk out done`。STT → `memo.text.set` も成功。
  同セッション 2 回目以降の転送は ACK 間隔が伸びて 60s 超になる
  (`ack timeout`)。アプリ側で音声をキャッシュして再転送を回避するようにした。

状態: 修正済み (実機で転送成功。2 回目以降はアプリのキャッシュで回避)

## T31. 音声メモの文字起こし (時計 + 電話) — 難度 M (実機)

- 方針: 時計単体での音声認識は現実的でない (ESP32-S3 8MB PSRAM)。
  電話側の STT (`AgentApi.transcribe` = OpenAI 互換 /audio/transcriptions) を使い、
  時計は要求と結果の保存だけを持つ。
- 時計: メモ詳細の「文字起こし」→ `ActionType::MemoTranscribe` →
  EVT `memo.transcribe.request {id}`。電話が `memo.text.set {id,text}` を返すと
  Voice メモの text に保存し、一覧・詳細に本文を出す。
- 電話: EVT を受けたら `memo.audio.get` + BULK で音声を取得 → STT →
  `memo.text.set`。メモ画面の「文字起こし」ボタンからも同じ処理を呼べる。
- 検証: core 230 件、firmware build、Android の protocol/agent テストと assemble/lint。
- 実機 (2026-10-24): logcat `transcribe result: 12 chars` → `memo.text.set: ok`
  → `EVT memo.saved` を確認。文字起こしは音声メモの `text` フィールド
  (NVS、最大 256B UTF-8) に保存され、時計の一覧・詳細に出る。
  電話は音声を `cacheDir/memo_audio/memo_<id>.adp` にキャッシュするだけで、
  本文の正は時計。長文は 256B で切り詰める。

状態: 完了 (2026-10-24 実機で確認)

## T32. 重いテーマで UI が固まる — 難度 H (実機・再現待ち)

- 症状 (ユーザー報告 2026-10-22): 重いテーマで固まり、強制電源 OFF しないと復活しない。
- 試したこと: テーマ巡回 (`t`) + bench (`b`) を 5 周 (mame/cosmos/cute/cyber/standard)
  → 再現せず (panic/assert なし、各周で health 応答あり、監査も 0 件)。
- 次に必要な情報: テーマ id、固まる操作 (画面遷移/スクロール/テーマ適用/BLE 接続)、
  フリーズ中にシリアル `h` が応答するか、画面は最後の描画のままか黒か。
- 候補: cute/cyber の bake (~850KB) 失敗経路、テーマ適用中の PSRAM 確保、
  BLE からのテーマ転送 (T25) との複合。
- 追加の証拠 (2026-10-22、cute で bench 8 周): 毎周
  `lv_draw_buf_create_ex: No memory: 346x615 ... 851160Byte` (アプリ一覧の bake、
  初回+リトライの 2 回) と `240x410 ... 393600Byte` が出る。画面は固まらず
  ライブ描画にフォールバックするが、PSRAM の断片化が進む。
  `W shell: event async defer fail` が 1 周あたり 20 件前後 — LVGL ロックの
  保持が長い (50ms タイムアウト)。
- 次の見方: 固まった瞬間のシリアルログ (No memory / defer fail / heap) と
  残り PSRAM。画面ブロック 640KiB×2 とアリーナの見直しは bench 前後比較が必要。

真因と修正 (2026-10-24、ユーザー再現: cyber 適用中に「戻る」でフリーズ):
- 症状: 画面が固まり PWR 短押しも無反応。シリアル `h` は応答し
  `app_loops` は増えるが、`task_wdt` が CPU1 の `taskLVGL` を検出。
- バックトレース: `lv_display_refr_timer` → `lv_obj_update_layout` →
  `lv_obj_get_screen`。画面ツリーの親チェーンが循環して無限ループしていた。
- 原因: `shell.cpp` の `drop_screens_now()` がテーマ切替時に**アクティブ画面
  (`disp->act_scr`) を直接 `lv_obj_delete`** していた。削除直後の画面参照が
  壊れ、次のレイアウト更新で親チェーンが循環する。PWR が効かないのは
  アプリタスクが LVGL ロック待ちで止まるため。
- 修正: 何も持たない一時画面を先に `lv_screen_load` し、保留中の削除
  タイマーと旧画面をその後で削除する (`act_scr` は一時画面を指したまま)。

追加の真因と修正 (2026-10-24 夜、ユーザー再現: テーマ変更直後に戻るを2回):
- 症状とバックトレース: `taskLVGL` が `lv_indev_search_obj` を無限ループ。
  2回目のタップのヒットテストが壊れたツリーを歩き続ける (WDT)。
- 原因: 1回目の遷移アニメが旧画面 (例: 設定) に残ったまま、シェルの
  `del_old_screen` がその画面を削除していた。LVGL の `lv_screen_load_anim` は
  連続ロード時に `act_scr` と新画面のアニメしか止めず、旧画面の `a_old` は
  残る。アニメは `set_x_anim` で解放済みメモリに座標を書き続け、新しい画面の
  親子リンクを壊す。
- 修正: `del_old_screen` と `drop_screens_now` で削除前に `lv_anim_delete(old)`。
  `d->act_scr == old` の間も削除を待つ。
- 検証: 「More → 設定 → cyber → 戻る2連打」で `tree del: ok`、WDT 0。
  テーマ変更の各段階と削除後に画面ツリーの親チェーンを検査する診断を追加
  (`tree ... ok/BAD` ログ)。

状態: 修正済み (2026-10-24、実機の手動2連打確認待ち)

## T33. テーマ文字盤「そぴあ」(ユーザー依頼 2026-10-25) — 難度 H

- 依頼: 参考イラストに寄せる — 立ち絵 (宇宙飛行士の女の子) を大きく左に、
  大きな数字を縦に積み、数字は立ち絵の後ろ、フォントは角張ったポップ体で等幅。
- 字盤テーマ `themes/sopia/`:
  - `bg.png` (全画面共通の星空) / `home_bg.png` (Home 専用の星域) / `chara_big.bin`
    (全身イラストから胴上を切り出して透過、410x502) / マスコット 3 表情。
  - 数字は `digits_96.bin` (Bungee 96px)。Bungee の数字は等幅でないため
    `tools/font_mono.py` で hmtx を共通送り幅に直してから binfont にした。
  - `face_layout`: `time_h`/`time_m` (96px, 縦積み, `behind:1`, `outline`)、
    `date` (shadow)、`battery` + `battery_gauge` (上右)、`steps` (下右)
- v6.1 キー (LVGL 側の追加実装):
  - `outline` / `outline_w`: 全周縁取り。同フォントのラベルを上下左右に
    `outline_w` だけずらして本体の親チェーンに積む (影→縁取り→本体)。
    LVGL の `text_outline` は vector graphics 無効で使えないための代替。
  - `behind`: chara より背面に描く (時計の数字の上に麦やリングが重なる)。
  - `set` を uint32 に広げて bit16..18 を使用 (manifest.hpp / manifest.cpp /
    tools/theme_build.py)。
- sim: テーマ巡回の `90_theme_custom_home` が `SetFace("bold")` のままで
  テーマ文字盤を撮れていなかったため `"theme"` に直し、アプリ一覧 (92) と
  タイマー (93) も撮るようにした。
- 検証: core 232 件、sim 監査 0 件 (docs/screenshots/96..99)、firmware build
  (app 空き 31%)、実機へアプリ + assets を書き込み済み。

追い込み (2026-10-25 ユーザー指摘: 数字を大きく/縦長、原色べた塗りがダサい、
歩数をもっと左、電池の数値とゲージを重ねる):
- 数字: Bungee を `font_mono.py --yscale 1.14` で縦長化し、`--size 112` で
  digits_112.bin (96px の 1.17 倍の高さ)。`face_layout.*.font = 112`。
  幅はそのまま (1 文字 69.8px) なので右端 374px で安全域に収まる。
- 塗り: `highlight` (v6.2) で上端にクリーム色の光、外周は白い `outline` 3px。
  塗り #F2B33D / 上光 #FFF6D8 (原色のべた塗りを避ける)。
- 電池: `battery` (テキスト) を `battery_gauge` の矩形に重ねて内側に % を出す。
  描画順は gauge → テキスト (build() で gauge を先に作る)。
- 歩数: 立ち絵レイヤー (compose_layer.py) に焼く下地を [222,434,392,492] に
  広げ、テキストを x=236 で中央寄せ (右端 377 = 余白 32 の内側)。
- 検証: core 232 件、sim 監査 0 件 (再撮影)、firmware build、実機書き込み。

2 回目の指摘 (2026-10-25: 数字の右が空いている/色がそぴあっぽくない/
電池 % が中央でない/% は不要/歩数は右揃え):
- 数字: 156px (`--size 156`, yscale 1.14) に拡大。x=−32 の右揃えではなく
  安全域基準で x=181 (箱の右端 376) に置き、右端近くまで使う。
- 色: 立ち絵の主要色 (k-means) に合わせて塗り #8E8AF0 (ペリウィンクル)、
  上光 #EFE9FF、縁取り #7FE4F7 (彼女のシアン)。日付は #D9E6FF。
- 電池: `%` をやめ数値だけに。テキスト要素の `w` で枠を固定して中央寄せ
  (w=78 をゲージと同じ矩形に)。
- 歩数: `x:-32, y:-32` の右端/下端基準 (右揃え) 。
- 判明した不具合と修正:
  - 右/下基準の要素は空ラベルのサイズで align されるため、テキスト確定後に
    画面外へはみ出していた → `settle()` でテキスト確定後に絶対座標で置し直す。
  - その後のクランプも `lv_obj_set_pos` だけでは align 再適用で戻される
    → 位置は `LV_ALIGN_TOP_LEFT` の絶対座標で確定させる。
  - `font` の許可値を列挙から範囲 (8..200) に変更。
- 検証: sim 監査 0 件、core 232 件、firmware build、実機書き込み。

3 回目の指摘 (2026-10-25: 数字をもっと右へ縁ギリギリまで / 歩数の黒帯を削除):
- 数字: 文字盤のラベルに `LV_OBJ_FLAG_USER_1` を付け、sim の余白監査を
  この要素だけ 6px にした (角で多少欠けても表示用として許容)。
  `x=207` で箱の右端 403px = 画面端から 7px まで使う。
- 歩数: 立ち絵レイヤーに焼いていた半透明の黒帯 (compose_layer.py) を廃止。
  文字は白 + 濃い縁取り 3px + 影で立ち絵の上でも読めるようにした。

状態: 実機で見た目確認待ち

## T36. テーマ「星夜もる」の追加 + タップで絵を切替 (ユーザー依頼 2026-10-28) — 難度 H

- 依頼: そぴあとは別のテーマを追加。頂いた素材は `/home/sasnews/Downloads/mol`
  (face-body.jpg, header.jpg, 配信サムネ 5 枚)。トップをタップすると画像が
  切り替わる。色合いと背景も手を入れる。試験管・実験器具・セロトニン・
  硫酸銅五水和物など「星夜もるらしい」要素を追加する。
- 素材: `face-body.jpg`（実験室 + バスト + 全身）から 410x502 の縦長を切り出し、
  青へ寄せるグレーディング (チャンネルゲイン → コントラスト → 周辺減光 →
  照明 → 星) を施した。`themes/mol/chara1.png` (バスト) / `chara2.png` (全身)、
  他画面用に `bg.png` (ぼかし)。
- モチーフ: `ic_tube` (試験管) / `ic_kipp` (キップの装置) / `ic_serotonin`
  (セロトニンの骨格) / `ic_crystal` (硫酸銅五水和物の結晶) を PIL で描いて
  PNG 化 (60x60, 半透明)。右端に HUD 風に縦並びで置く。
- レイアウト: 数字は左の棚の上 (84px, Poppins を等幅化 + 1.06 縦伸ばし)、
  日付と電池は右上、歩数は右下。立ち絵は全画面を覆うので数字は手前に置く。
- ファーム: `assets` の id が `chara` で始まるものを集め、
  文字盤のタップで次の絵へ切り替える (`chara_tap_cb`、`theme_image()` で
  遅延ロード)。テーマを変えると先頭に戻る。
- 検証: sim 監査 0 件 (`docs/screenshots/96..97_theme_mol_*`)、core テスト、
  firmware build、実機書き込み。PSRAM は全画面 2 枚 (~820KB) + bg で
  sopia と同程度に収まる (実機の `h` で確認)。

- 追記 (2026-10-28): 「テーマを選んでもトップに立ち絵が出ない」報告。原因は
  文字盤設定 (settings.face) がテーマと別で、既定の bold のままだったこと。
  `shell.cpp` の `adopt_theme_face()` で、face_layout を持つテーマを選んだら
  face を `theme` に切り替える。工場出荷の bold のままなら起動時も採用。
状態: 実機で見た目とタップ切替の確認待ち

## T37. 歩く大会モード (ウォーク) — ユーザー依頼 2026-10-29 — 難度 H

- 依頼: 決まった道を歩く大会用のアプリ。進捗 % と開始時刻からの到着予想時刻を
  時計に出す。ルートは何かで設定し、基本はスマホアプリで管理する。
  測位を動かし続けると電池が切れるので、時計を開いたときだけ時計がデータを
  要求する形にする (時計側の電池持ちを優先)。
- 方針: **スマホがルート・測位・進捗/ETA の正**。時計は表示だけで、GNSS も
  常時通信もしない。時計は「ウォーク画面が Active で見えている間」だけ
  10 秒に 1 回 EVT `walk.query` を送り、スマホが REQ `walk.state` で返す。
  Dim / ScreenOff / 別画面では送らない。画面を開いた瞬間は即時要求。
  測位はスマホのみ・歩行セッション中だけ (GPS 10s/10m、Foreground Service)。
- 実装:
  - 時計: `apps/walk/` (Feature + 画面 + テスト)、`core/include/watch/features/walk.hpp`。
    `walk.state` を protocol に追加 (`dispatch.cpp`、`hello` caps に `walk`)。
    アプリ一覧のサブ行に「62% ・ 到着 14:35」を出す。アイコンは Material
    Symbols "hiking" (0xE50A) をアイコンフォントに追加。
  - スマホ: `android/walk` (純 Kotlin: GPX パーサ・投影・ETA) + `WalkRepository`
    / `WalkTracker` / `WalkService` (FGS location) / `WalkScreen`
    (GPX 取込・ルート選択・Canvas スケッチ・開始/終了)。デバイスタブの
    カードから開く (ボトムタブは増やさない)。
  - 仕様・使い方・電池の説明: `docs/walk-event.md`。BLE 仕様は
    `docs/protocol-v1.md` の「歩く大会 (walk)」節。
  - 既存バグ修正: `shell.cpp` の `kRouteScreen[]` が Route 0..18 の 19 要素で、
    自動採番アプリ (App0 = 32) の画面で範囲外参照になっていた。40 要素に拡張し、
    `route_name()` がアプリ id を返すようにした。
- 受け入れ条件: core 全テスト / sim の安全域監査 0 / `:walk:test :protocol:test
  :agent:test :app:assembleDebug :app:lint` 成功 / firmware build 成功。
- 実機確認待ち: 時計⇄スマホの walk.query / walk.state 往復、hiking アイコンの
  描画、10 秒ポーリングの電池影響、Android 14 の FGS(location) と GPS の実挙動。

状態: 完了 (2026-10-29、実機確認待ち)

## 第2弾の実施順

T16 → T17 → T18 (実機・ユーザー操作) → T19 → T20 → T21 → T22 は完了または進行中。

現状サマリ (2026-10-22): T27 完了、T29 (動作が重い) 完了 — スキン近似塗りと
LVGL ヒープの PSRAM 化で cute/cyber も 80〜160ms まで改善 (docs/perf.md)。
T24 も実機検証済み (app 空き 8%→31%、日本語フォントは assets の binfont)。
T18 は録音経路を修復し、UI のメモ画面から録音・保存できることを実機確認済み。
T28 (設定画面のファイルテーマ選択) は実装済みで、実機は書込み・起動・
設定画面生成まで確認済み (タップと再起動持続だけユーザー確認待ち)。
T25 (BLE テーマ転送) と T30 (音声メモのスマホ転送) は先送り。T22 はユーザーの判断待ち。
歩数計の HW ペドメータ修正は実機で歩数増加を確認済み (2026-10-23)。

次のおすすめ順: **T28 の実機確認 → T25 → T22**。

## 対象外 / 判断済み

- D「電話側は読み込み専用」: 棚卸しの結果、電話はテキストメモの作成・削除が既にできる。追加作業なし。
- touch NACK (~13.8s 周期で1回): T29 の続きで解消 (FT3168 スリープ中のポーリングを GPIO38 ゲートで停止)。
- light sleep の本格実装: 消費電力に効く本命だが、影響範囲が大きい。
  実機の電流計測環境ができてから別途扱う (imu の `arm_light_sleep_imu` に TODO 済み)。

## 実施順

依存関係と実機確認のまとめやすさで並べる:
T0 → T1, T2, T3 → T5 → T4 → T7 → T6 (ユーザー承認) → T9 → T10 → T11 → T12 → T13 → T8 → T14 → T15。
実機確認が要る T5/T9/T10/T11/T12 は、2〜3 件ずつまとめて書き込み、まとめてユーザーに確認を依頼する。
