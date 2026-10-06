# theme-format.md — テーマパッケージ形式 v3

`docs/plan.md` H 章 (Theme) と I 章 (Asset) の実装形式。
スマホから BLE BULK (`kind:"theme"`) で時計に送られ、`assets` パーティション
(littlefs、VFS では `/assets` にマウント) の `themes/<id>.zip` に保存される
(v2 からは**展開しないで zip のまま**保持。詳細は「転送・有効化フロー」)。
旧形式で `themes/<id>/` に展開済みのテーマも後方互換で読む。

## パッケージ (`.zip`, stored のみ)

転送サイズを BLE で無駄にしないため zip コンテナにするが、
**圧縮エントリは受け付けない (method=STORED のみ)**。
時計側は miniz 等を持たず、セントラルディレクトリのオフセット表を読んで
各エントリを直接参照する (展開コピーを作らない)。
`tools/build_themes.py` がこの形式で出力する。

```
<mame>.zip
  manifest.cbor     必須
  <image>.bin       LVGL バイナリ画像 (RGB565 / RGB565A8)
  <image>.png       PNG (v2〜、読み込み時に RGB565(A8) に変換)
  <font>.bin        LVGL バイナリフォント (lv_binfont 形式、v2〜)
```

エントリ名は `[a-z0-9._-]` のみ・ディレクトリ不可・`..` 不可・**最大 32 個**。
パッケージ全体は **4 MiB まで** (受信側がリジェクトする上限)。
旧版は「staging zip + 展開済みコピー」の二重占有で実質 ~2.7MiB が天井だったが、
v2 は zip 自体をそのまま配置するので上限 = パーティションの空きだけ。

## manifest.cbor (CBOR map)

```jsonc
{
  "id":      "cosmos",      // 必須。theme id: [a-z0-9-]{1,31}
  "api":     1,             // 必須。形式バージョン (1 のみ受理)
  "name":    "cosmos",      // 表示名。省略時 id と同じ
  "version": 1,             // パッケージ版。省略時 1
  "tokens": { /* 色・サイズ (下表) */ },
  "images": { /* 基本画像スロット (v1) */ },
  "bubble": { /* chara_bubble ふきだし文言 (v1) */ },
  // ---- v2 拡張 (全部任意) ----
  "screens":     { /* 画面ごとの背景+スクリム */ },
  "style":       { /* コンポーネントスキン */ },
  "icons":       { /* アプリ id → アイコン画像 */ },
  "fonts":       { /* binfont フォント差替え */ },
  "mascot":      { /* 画面隅マスコット */ },
  "face_layout": { /* "theme" 文字盤の要素配置 */ }
}
```

- 未知キーは無視 (将来互換)。v2 キーを読めない古いファームは色だけの
  テーマとして動き続ける。
- `tap_min` はテーマ不可 (最小タップ 48dp は利用性のハードルール)。

### tokens (v1)

```jsonc
"tokens": {
  "bg":         "0x000000",   // 色は "0xRRGGBB" テキスト
  "surface":    "0x14161D",
  "surface2":   "0x1F222C",
  "line":       "0x2A2E3A",
  "primary":    "0xFF8A3D",
  "primary2":   "0xFFB27A",
  "on_primary": "0x1A0D00",
  "text":       "0xF4F4F6",
  "text_dim":   "0x8A8F9C",
  "accent":     "0xFF8A3D",
  "danger":     "0xFF5C6C",
  "ok":         "0x3DDC97",
  "accent2":    "0xE8D7B0",   // 第2アクセント (analog の針・数字など)
  "accent3":    "0x00E0C6",   // 第3アクセント (HUD の飾り・バーなど)
  "accent4":    "0x7A5CFF",   // 第4アクセント (グロー・日進捗の弧など)
  "accent5":    "0xFF5C8A",   // 第5アクセント (キャラ文字盤の強調)
  "bubble_bg":  "0xFFFFFF",   // chara_bubble ふきだしの背景
  "bubble_text":"0x2B1D45",   // chara_bubble ふきだしの文字色
  "line":       "0x2A2E3A",   // リスト区切り線・スライダートラック
  "primary2":   "0xFFB27A",   // primary グラデーション終端
  "edge":       "0x4A4F5C",   // 行末 chevron・スイッチOFFトラック
  "radius_sm": 10,        // uint
  "radius_lg": 22,        // uint
  "space":     8,         // uint 基本余白
  "anim_ms":   220,       // uint 画面遷移 ms
  "font_body":  20,       // uint。内蔵フォントから選ぶ
  "font_title": 26,       //   20|26|56|96 のみ有効
  "font_digits":96,
  "font_digits_sm":56
}
```

### images (v1 基本スロット)

```jsonc
"images": {
  "home_bg":    "home_bg.bin",
  "stand":      "stand.png",
  "timer_done": "timer_done.png",
  "face_chara": "face_chara.png"
}
```

| slot | 使う画面 | 推奨サイズ | 上限 |
|---|---|---|---|
| `home_bg` | Home 背景 (時刻の背後・下帯) | 410x240 | 410x502 |
| `stand` | Home 立ち絵 (右下) | 160x200 | 240x360 |
| `timer_done` | タイマー終了アラート中央 | 320x240 | 410x320 |
| `face_chara` | 文字盤 `chara_*` / `theme` の立ち絵 (透過 RGB565A8 推奨) | 240x410 | 240x410 |

- `face_chara` が無いテーマでは chara_* 文字盤は `stand` を **拡大せず**
  そのまま使う。どちらも無ければ「キャラ画像なし」の簡易レイアウト
  (時刻中央) にフォールバックする。

### bubble (v1)

```jsonc
"bubble": {               // chara_bubble 文字盤のふきだし文言
  "morning": "おはよう！",      // 朝 (5-10時)
  "noon":    "こんにちは！",     // 昼 (10-16時)
  "evening": "おつかれさま！",   // 夕 (16-19時)
  "night":   "おやすみー",      // 夜 (19-5時)
  "steps":   "あと{n}歩だよ"   // 歩数目標の残り行。"{n}" に残り歩数を埋める
}
```

各文言は48バイト (UTF-8) まで。省略したキーは内蔵の既定文を使う。
`steps` は歩数・目標が両方分かり、残りがある時だけ2行目として使われる。

## v2 キー詳細

### `screens` — 画面ごとの背景 + スクリム

```jsonc
"screens": {
  "*":        {"bg": "bg.png", "scrim": 110},
  "timer":    {"bg": "timer_bg.png", "scrim": 60},
  "alert":    {"bg": "alert_bg.png", "scrim": 70},
  "settings": {"scrim": 150}
}
```

- キーは画面名: `home` `quick` `more` `timer` `stopwatch` `counter`
  `steps` `memo` `settings` `ota` `powermenu` `alarm` `notifications`
  `media` `agent` `alert` (タイマー終了/アラームの全画面オーバーレイ)、
  および `"*"` (全画面フォールバック)。
- `bg` = 画面背景画像 (`.bin` / `.png`、410x502 推奨)。画面を開くとき
  PSRAM にロードされ、閉じる/遷移するとき解放される (常駐しない)。
- `scrim` = 背景の上に敷く黒レイヤの不透明度 0-255 (省略 0)。
  透過カード (style.card_opa) と組み合わせて読みやすさを調整する。
- `settings` のように `bg` を書かないキーは「`*` の背景のまま
  スクリムだけ変える」指定になる。

### `style` — コンポーネントスキン

```jsonc
"style": {
  "card_radius": 12,          // グループカードの角丸 (省略時 tokens.radius_lg)
  "card_opa":    150,         // カード不透明度 0-255 (省略=255)
  "border_w":    1,           // カード枠線の太さ px
  "border":      "0x3A5F8A",  // 枠線色
  "glow_color":  "0x2A6FC0",  // primary ボタンのグロー色
  "glow_w":      14,          // グロー幅 px (0 で無効)
  "btn_radius":  8,           // ボタン角丸 (省略=ピル)
  "header":      "flat"       // "default" | "flat" (flat=戻るピル無し・HUD風)
}
```

コンポーネント (カード・行・ボタン・ヘッダ) がハードコード値の代わりに
これを読む。`card_opa` を下げると `screens` の背景が透けて見える。

### `icons` — アプリ一覧のタイル差替え

```jsonc
"icons": {
  "timer": "ic_timer.png",
  "stopwatch": "ic_stopwatch.png"
}
```

- キーはアプリ id (`timer` `stopwatch` `counter` `memo` `alarm`
  `notifications` `media` `steps` `agent` `settings` ほか、最大12件)。
- 画像は 36x36 前後の透過 PNG 推奨。無いアプリは従来の色タイル+
  頭文字にフォールバック。

### `fonts` — binfont フォント差替え

```jsonc
"fonts": {
  "digits":    "digits.bin",    // 時計・タイマーの大数字
  "digits_sm": "digits_sm.bin", // 中数字
  "title":     "title.bin",     // ヘッダタイトル
  "body":      "body.bin"       // 本文
}
```

- `.bin` は LVGL バイナリフォント (`lv_font_conv` 出力、lv_binfont 形式)。
  受信時の前置きなしで `lv_binfont_create_from_buffer` に直渡しされる。
- 日本語グリフを含まないフォントを `body`/`title` に入れると
  日本語が欠けるので注意 (cosmos は digits 系のみ差替え)。
- フォントデータはテーマ適用中ずっとアリーナに常駐する (遅延解放なし)。

### `mascot` — 画面隅マスコット

```jsonc
"mascot": {
  "x": 300, "y": 380,                        // 画像左上の画面座標 (負値可)
  "expr":  {"normal": "m_n.png", "smile": "m_s.png", "wink": "m_w.png"},
  "lines": ["宇宙を見てるよ", "おつかれさま", "きらきら〜", "タップありがと"],
  "screens": ["more", "memo", "timer"]     // 出す画面。"*"=全画面
}
```

- `expr` は表情名→画像の map (最大4種・各96x96程度)。タップで
  表情とセリフが次に切り替わる。
- `lines` はタップごとにランダム選択されるセリフ (最大8件・各48B)。
- `screens` は有効化する画面名リスト (省略 = 出ない)。
- 表示モードは画面で自動切替: ヘッダがある画面 (リスト系) では
  **~44px に縮小してヘッダ右側** (右端から22px・タイトルと同じ高さ)
  に出し、コンテンツとは一切重ならない。ヘッダの無い画面
  (face/alert などレイアウトが場所を確保するもの) だけ
  `x`/`y` 指定の原寸表示になる。
- ヘッダ画面でタップするとセリフがヘッダ直下の右上に小さな
  トーストとして出て 3 秒で自動で消える。隅表示では画像直上の
  吹き出し。
- 原寸表示の `x`/`y` は丸角 (~44px) より内側の安全域
  (各端から22px) にクランプされる。右下に置くなら負値オフセットが便利。
- タップは画像の矩形内だけが有効。吹き出し/トーストはタップを取らない。

### `face_layout` — `theme` 文字盤の要素配置

テーマが `face_layout` を持つと、設定の文字盤一覧に「テーマ」
(face id `theme`) が現れ、manifest だけで新しい文字盤を組める。

```jsonc
"face_layout": {
  "time":    {"x": 30,  "y": 96,  "font": 96, "color": "0x9AE8FF"},
  "date":    {"x": 32,  "y": 200, "font": 20, "color": "0x7E95B8"},
  "steps":   {"x": 32,  "y": 228, "font": 20, "color": "0x7E95B8"},
  "battery": {"x": -30, "y": 96,  "font": 20, "color": "0x7E95B8"},
  "notify":  {"x": -30, "y": 124, "font": 20, "color": "0xFFB45C"},
  "bubble":  {"x": 30,  "y": 32,  "font": 20, "color": "0xCFEAFF"},
  "chara":   {"x": 176, "y": 120, "img": "face_chara.png"}
}
```

- 要素: `time` `date` `steps` `battery` `notify` `bubble` `chara`。
- `x`/`y` は要素左上の座標。**負値は右端/下端基準** (`x:-30` = 右から30px)。
- `font` は px サイズ (`fonts.digits` 等の差替えフォントがあればそれが優先)。
- `w`/`h` で枠サイズを固定できる (0/省略 = 内容に合わせる)。
- `chara.img` で立ち絵を指定 (省略時 `images.face_chara`)。
- 全要素省略なら既定レイアウトにフォールバック。
- 座標は丸角の内側 (左右 22px 以上・上端 ~30px 以上) に収めるのが
  無難。要素同士が重ならないようピクセル単位で確認すること。

## v3 キー詳細

### `skin` — 画像スキン (9-slice)

コンポーネントの見た目を PNG 画像で差し替えるキー。パーツごとの画像は
9-slice (角=原寸、辺/中央=伸縮) で任意のサイズの部品に描画される。
全部任意: 未指定のパーツは従来のベクタ描画のまま (`style` キーと併用可)。

```jsonc
"skin": {
  "button_primary": {
    "img": "button_primary.png",      // 必須 (パーツ画像)
    "slice": [28, 33, 28, 33],        // [左,上,右,下] の不変幅 px (省略=0: 全面伸縮)
    "states": {                        // 状態ごとの差替え画像 (任意)
      "pressed":  "button_primary_pressed.png",
      "checked":  "button_primary_on.png",
      "disabled": "button_primary_disabled.png"
    },
    "pad": [10, 0, 10, 0],            // コンテンツの内側余白に加算 (任意)
    "text": "0x02101F",               // パーツ内の文字色 (任意)
    "text_pressed": "0x02101F"        // 押下中の文字色 (任意)
  }
}
```

| パーツ | 対象 |
|---|---|
| `card` | パネルカード |
| `list_group` | 行をまとめるグループカード |
| `row` | 一覧の行 (+`pressed` 可) |
| `divider` | 行間の区切り線 |
| `button_primary` / `button_secondary` / `button_danger` | 全幅ボタン (+`pressed`/`disabled`) |
| `back_pill` | ヘッダの「‹ 戻る」ピル (+`pressed`) |
| `header_bar` | ヘッダ帯 (410x76) |
| `switch_track` / `switch_knob` | スイッチ (+`checked` = ON) |
| `slider_track` / `slider_fill` / `slider_knob` | スライダー |
| `icon_tile` | 行アイコンの 36x36 タイル |
| `toast` | マスコットのセリフ吹き出し |
| `bubble` | 文字盤の吹き出し |
| `caption_line` | セクション見出し下のアンダーライン (任意) |

- `slice`: 画像の左/上/右/下端を固定する幅 (px)。角はそのまま、辺と中央が
  伸縮する。合計が対象サイズを超えるときは自動で縮む (潰れない)。
- `states`: LVGL の state 名。未指定の状態は `img` にフォールバック。
- `pad`: 部品の padding に加算される (画像の飾りが文字に被らないよう)。
- 状態切替は画像差替えのみ (LVGL state で即座に切り替わる。再生成なし)。
- skin 画像はテーマ適用時にまとめてグローバル領域へロードして常駐。
  小さいパーツ群前提 (サンプルは ~650 KB/テーマ)。
- 角丸・角切り・影は画像の α をそのまま使うので自由な形が作れる。
- 画像ファイル名は 32 文字まで (`kThemeImageNameMax`)。

### キット (kit.html) でスキンを作る

`themes/<id>/kit.html` に全部のパーツを CSS で描き、
`tools/build_themes.py --kit` が headless Chromium (Playwright) で
要素ごと透過 PNG スクショ → manifest の `skin` ブロックを自動生成 →
zip まで一括で作る。サンプルは `themes/cyber/kit.html` / `themes/cute/kit.html`。

#### 使い方 (クリック単位)

1. `pip install playwright`
2. `playwright install chromium`  (初回だけ。headless Chrome を落とす)
3. `themes/<id>/kit.html` を書く (`themes/cyber/kit.html` をコピーするのが早い)
4. `python3 tools/build_themes.py --kit <id>` を実行
   → `sim/themes/<id>.zip` (sim が読む) + `sim/themes/<id>/` 展開 +
   `android/app/src/main/assets/themes/<id>.zip` + `tools/themes/<id>.zip`

kit.html の書き方:

- スキンにしたい要素に `data-part="<パーツ名>"` を付け、実寸 (watch px) の
  `width`/`height` を style で指定する。要素の領域がそのまま PNG になる。
- `data-slice="l,t,r,b"` : slice 値 (その PNG 上の px)。
- `data-pad="l,t,r,b"` : pad (省略可)。
- `data-text="0xRRGGBB"` / `data-text-pressed="0xRRGGBB"` : 文字色 (省略可)。
- 状態違いは別要素に `data-state="pressed|checked|disabled"` を付けて作る。
- スキン以外の画像は `data-asset="file.png"` でそのまま PNG 出力される
  (画面背景 `bg.png` などに使う)。
- 要素の背景を透過にしておけば PNG は α 付きで切り抜かれる。
  グロー/影 (box-shadow, drop-shadow) が要素の矩形をはみ出ると切れるので、
  外側に透明パディングを入れる (cyber/cute の `--m` 変数の要領)。
  slice 値はこの透明パディングを含めた PNG 上の px で指定すること。

#### Figma から持ってくる場合

Figma で各パーツを PNG 書き出ししてファイル名を合わせれば kit.html は不要:

- ファイル名: `<part>.png` (通常) / `<part>_<state>.png` (状態)
  例: `button_primary.png`、`button_primary_pressed.png`
- `slice`/`pad`/`text` は自分で manifest.cbor に書くか、
  `--kit` が生成した `sim/themes/<id>/manifest.cbor` を参考にする。

## 画像形式

### `.bin` = LVGL バイナリ画像 (v1)

`lv_image_dsc_t` を直接作れる生形式 (LVGL 9.5 の `lv_image_header_t`):

```
offset  size  内容
0       1     magic = 0x19
1       1     cf (0x12=RGB565 / 0x14=RGB565A8)
2       2     flags = 0
4       2     w (LE u16)
6       2     h (LE u16)
8       2     stride = w*2 (LE u16)
10      2     reserved = 0
12      …     画素データ
```

- `RGB565`   : `w*h*2` バイト (1px=2B LE)。不透明。
- `RGB565A8` : `w*h*2` バイトの色配列 + `w*h` バイトの α 配列。`data_size = w*h*3`。

### `.png` (v2)

通常の PNG (8bit RGBA/RGB)。時計側は読み込み時に LVGL 同梱の
**lodepng** で一度だけデコードし、RGB565 / RGB565A8 に変換してから
`lv_image_dsc_t` を作る (LVGL デコーダキャッシュには依存しない)。

## ロード/解放ポリシー (v2)

テーマ用 PSRAM アリーナ (3 MiB) を「グローバル」と「画面背景」の
2領域に分けて使う:

- **グローバル領域 (~1.6 MiB)**: manifest 適用時に `images` 4スロット、
  `icons`、`mascot` 表情、`fonts`、face 用 chara、**`skin` パーツ画像**
  (9領域に分割した RGB565A8) をまとめてロード。
  テーマを変えるまで常駐。
- **画面背景領域 (2ブロック x 640 KiB)**: `screens.<画面>.bg` は
  画面生成時にだけロードし、`LV_EVENT_DELETE` (閉じる/遷移) で解放。
  ナビ中は現画面 + home しか生きないので2ブロックで足りる
  (410x502 RGB565A8 = 617 KiB/枚)。
- いずれかがアリーナに収まらない場合はその要素だけスキップし、
  テーマ自体はフォールバック描画で適用される (全落ちしない)。

## 転送・有効化フロー

1. Phone → `BULK_START{id,kind:"theme",size,sha256,chunk}` (protocol-v1.md BULK 章)。
   再開時は同じ `id` で `BULK_ACK{next}` に続きから送る。
2. 時計は `/assets/.theme.bulk` に受け、`BULK_END` で sha256 照合。
   一致 → zip 検査 (上記) → **`/assets/themes/<id>.zip` として移動**
   (展開しない) → theme id を適用待ちに。
3. `settings.set {theme:"<id>"}` でも切替え可 (BLE かウォッチ UI)。
   どちらの経路も `SetTheme` Action → `ThemeChanged` Event →
   適用層が zip 内エントリまたは内蔵テーマを読み直す。
4. 読めない / 壊れたテーマは **内蔵 `standard` にフォールバック**
   (設定値はそのまま残し、起動ごとに再試行する)。

## 内蔵テーマ

| id | 内容 |
|---|---|
| `standard` | 既定ダーク (AMOLED 向け黒背景) |
| `light` | 明るめ (屋外視認用) |

キャラクターサンプル `mame` と v2 サンプル `cosmos` は内蔵せず、
`themes/*.zip` として配布/生成する (flash 節約)。

## 予算まとめ

| 項目 | 上限 | 根拠 |
|---|---|---|
| パッケージ | 4 MiB | 受信上限 (theme_store::kPkgMax)。zip を展開せず保持するので実質=空き容量 |
| 画素データ計 (常駐分) | ~1.6 MiB | グローバル領域 (アリーナ 3 MiB うち) |
| 画面背景 | 640 KiB/枚 x 2 | 画面生成時のみ PSRAM。410x502 RGB565A8 = 617 KiB |
| エントリ数 | 32 | ディレクトリ表の固定長 |
| `themes/` 使用量 | 6 MiB | assets パーティション |
| 画像1枚 | 410x502 まで | 画面サイズ |
| mascot lines | 8件 x 48B | ThemeMascot 固定長 |
| icons | 12件 | ThemeIcon 固定長 |
| skin パーツ | 18種 x 状態4枚 | ThemeSkinPart 固定長。実測 ~0.65 MiB/テーマ |
