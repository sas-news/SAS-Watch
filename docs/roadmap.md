# roadmap.md — 残タスクの実施計画 (2026-10-12 時点・引き継ぎ用)

`docs/handoff.md` の残タスク全部について**方針はすでに決めてある**。
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
    (ESP-IDF は `/home/sasnews/esp/esp-idf`)。書き込みは handoff.md「実機操作での失敗と教訓」の
    esptool 手順 (アプリは 0x20000)。
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

状態: T16 に吸収 (監査結果は docs/t6-audit.md)

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
  監査結果は handoff.md に残す。

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
  - T6 監査 (docs/t6-audit.md) の「確実な違反」のうち、数字の色に関するもの以外も直す。
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

状態: 進行中 (診断ログ入りを 85bedc2 で書き込み済・実機操作待ち)

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

状態: 未着手

## T24. flash の app 領域が残り 8% — 難度 M

- T16 のフォント補完で 0x2E→0x3A (+~770KB) 増えた。
- 方針: 日本語フォント (jp_20/26/32) を app バイナリから外し、assets パーティションの
  binfont (`lv_binfont_create`、PSRAM にロード) にする。起動時に 1 回読む。
  失敗したら最小の内蔵フォント (ASCII+かな) にフォールバックする。
  あわせて、パーティション表で app を拡張できる余地も確認する (32MB flash)。OTA の 2 面構成を保つこと。
- 受け入れ条件: app 領域の空きを 30% 以上に戻す。sim の監査 0 件を維持する。

状態: 未着手

## 第2弾の実施順

T16 → T17 → T18 (実機・ユーザー操作) → T19 → T20 → T21 → T22 は完了または進行中。次は **T23 → T18 の完了 → T24 → T22 の実装 (ユーザー承認後)**。

## 対象外 / 判断済み

- D「電話側は読み込み専用」: 棚卸しの結果、電話はテキストメモの作成・削除が既にできる。追加作業なし。
- touch NACK (~13.8s 周期で1回): 実害なし。対応しない。
- light sleep の本格実装: 消費電力に効く本命だが、影響範囲が大きい。
  実機の電流計測環境ができてから別途扱う (imu の `arm_light_sleep_imu` に TODO 済み)。

## 実施順

依存関係と実機確認のまとめやすさで並べる:
T0 → T1, T2, T3 → T5 → T4 → T7 → T6 (ユーザー承認) → T9 → T10 → T11 → T12 → T13 → T8 → T14 → T15。
実機確認が要る T5/T9/T10/T11/T12 は、2〜3 件ずつまとめて書き込み、まとめてユーザーに確認を依頼する。
