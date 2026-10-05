# theme-format.md — テーマパッケージ形式 v1

`docs/plan.md` H 章 (Theme) と I 章 (Asset) の実装形式。
スマホから BLE BULK (`kind:"theme"`) で時計に送られ、`assets` パーティション
(littlefs、VFS では `/assets` にマウント) の `themes/<id>/` に展開される。
VFS パスで言うと `/assets/themes/<id>/`。

## パッケージ (`.zip`, stored のみ)

転送サイズを BLE で無駄にしないため zip コンテナにするが、
**圧縮エントリは受け付けない (method=STORED のみ)**。
時計側は miniz 等を持たず、ヘッダ読み + CRC32 だけで展開する。
`tools/build_themes.py` がこの形式で出力する。

```
<mame>.zip
  manifest.cbor     必須
  <image>.bin       images で参照されるファイル (LVGL バイナリ画像)
```

エントリ名は `[a-z0-9._-]` のみ・ディレクトリ不可・`..` 不可・最大 16 個。
パッケージ全体は **3 MiB まで** (受信側がリジェクトする上限)。
時計は zip をメモリに置かず littlefs 上の `.theme.bulk` から
エントリごとにストリーミング展開する。

## manifest.cbor (CBOR map)

```jsonc
{
  "id":      "mame",        // 必須。theme id: [a-z0-9-]{1,31}
  "api":     1,             // 必須。形式バージョン (1 のみ受理)
  "name":    "まめ",         // 表示名。省略時 id と同じ
  "version": 1,             // パッケージ版。省略時 1
  "tokens": {               // 省略可。未指定は標準テーマ (standard) の値で補完
    "bg":         "0x000000",   // 色は "0xRRGGBB" テキスト
    "surface":    "0x131820",
    "surface2":   "0x1E2634",
    "primary":    "0x4DA3FF",
    "on_primary": "0x051018",
    "text":       "0xF2F5F8",
    "text_dim":   "0x8B95A3",
    "accent":     "0xFF8A3D",
    "danger":     "0xFF5C5C",
    "ok":         "0x46E29A",
    "accent2":    "0xE8D7B0",   // 第2アクセント (analog の針・数字など)
    "accent3":    "0x00E0C6",   // 第3アクセント (HUD の飾り・バーなど)
    "accent4":    "0x7A5CFF",   // 第4アクセント (グロー・日進捗の弧など)
    "accent5":    "0xFF5C8A",   // 第5アクセント (キャラ文字盤の強調)
    "bubble_bg":  "0xFFFFFF",   // chara_bubble ふきだしの背景
    "bubble_text":"0x2B1D45",   // chara_bubble ふきだしの文字色
    "radius_sm": 10,        // uint
    "radius_lg": 18,        // uint
    "space":     8,         // uint 基本余白
    "anim_ms":   220,       // uint 画面遷移 ms
    "font_body":  20,       // uint。内蔵フォントから選ぶ
    "font_title": 26,       //   20|26|56|96 のみ有効
    "font_digits":96,
    "font_digits_sm":56
  },
  "images": {               // 省略可。スロット名 → zip 内ファイル名
    "home_bg":    "home_bg.bin",
    "stand":      "stand.bin",
    "timer_done": "timer_done.bin",
    "face_chara": "face_chara.bin"
  },
  "bubble": {               // 省略可。chara_bubble 文字盤のふきだし文言
    "morning": "おはよう！",      // 朝 (5-10時)
    "noon":    "こんにちは！",     // 昼 (10-16時)
    "evening": "おつかれさま！",   // 夕 (16-19時)
    "night":   "おやすみー",      // 夜 (19-5時)
    "steps":   "あと{n}歩だよ"   // 歩数目標の残り行。"{n}" に残り歩数を埋める
  }
}
```

- 未知キーは無視 (将来互換)。
- `tap_min` はテーマ不可 (最小タップ 48dp は利用性のハードルール)。
- フォントは内蔵4種 (`font_jp_20` / `font_jp_26` / `font_digits_96` /
  `font_digits_56`) のサイズ指定のみ。フォント自体は配布しない
  (日本語サブセットで数百KBになるため)。
- `bubble` の各文言は48バイト (UTF-8) まで。省略したキーは
  内蔵の既定文を使う。`steps` は歩数・目標が両方分かり、残りが
  ある時だけ2行目として使われる (残り0または不明なら挨拶だけ)。

## 画像スロット

Screen 側が持つ差替え枠。テーマが画像を持たなければ従来通り色だけの画面。

| slot | 使う画面 | 推奨サイズ | 上限 |
|---|---|---|---|
| `home_bg` | Home 背景 (時刻の背後・下帯) | 410x240 | 410x502 |
| `stand` | Home 立ち絵 (右下) | 160x200 | 240x360 |
| `timer_done` | タイマー終了アラート中央 | 320x240 | 410x320 |
| `face_chara` | 文字盤 `chara_side` / `chara_bubble` の立ち絵 (透過 RGB565A8 推奨) | 240x410 | 240x410 |

- `face_chara` が無いテーマでは chara_* 文字盤は `stand` を **拡大せず**
  そのまま使う。どちらも無ければ「キャラ画像なし」の簡易レイアウト
  (時刻中央) にフォールバックする。

## 画像形式 (`.bin` = LVGL バイナリ画像)

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
- 全スロットの画素データ合計で **2.5 MiB まで**
  (適用時に PSRAM のテーマ用アリーナ 3 MiB に読み込まれ、`lv_image_dsc_t.data` が指す)。

受信側チェック: magic / cf / `stride==w*2` /
`data_size == w*h*(cf==0x14 ? 3 : 2)` / スロットごとの寸法上限。

## 転送・有効化フロー

1. Phone → `BULK_START{id,kind:"theme",size,sha256,chunk}` (protocol-v1.md BULK 章)。
   再開時は同じ `id` で `BULK_ACK{next}` に続きから送る。
2. 時計は `/assets/.theme.bulk` に受け、`BULK_END` で sha256 照合。
   一致 → zip 検査 (上記) → `/assets/themes/<id>/` へ展開 → theme id を適用待ちに。
3. `settings.set {theme:"<id>"}` でも切替え可 (BLE かウォッチ UI)。
   どちらの経路も `SetTheme` Action → `ThemeChanged` Event →
   適用層が `themes/<id>/` または内蔵テーマを読み直す。
4. 読めない / 壊れたテーマは **内蔵 `standard` にフォールバック**
   (設定値はそのまま残し、起動ごとに再試行する)。

## 内蔵テーマ

| id | 内容 |
|---|---|
| `standard` | 既定ダーク (AMOLED 向け黒背景) |
| `light` | 明るめ (屋外視認用) |

キャラクターサンプル `mame` は内蔵せず、Android アプリ同梱の
`themes/mame.zip` として配布する (flash 節約。
内蔵テーマに戻すだけなら `standard` / `light` でも十分)。

## 予算まとめ

| 項目 | 上限 | 根拠 |
|---|---|---|
| パッケージ | 3 MiB | 受信上限 (theme_store::kPkgMax) |
| 画素データ計 | 2.5 MiB | 画像は LVGL 適用時に PSRAM のテーマ用アリーナ (3 MiB) に保持 |
| エントリ数 | 16 | ディレクトリ表の固定長 |
| `themes/` 使用量 | 6 MiB | assets パーティション |
| 画像1枚 | 410x502 まで | 画面サイズ |
