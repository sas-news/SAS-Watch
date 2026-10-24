# theme-guide.md — テーマ作成ガイド

`theme.json` を書いて、プレビューし、実機に入れるまでの手順。
仕様の正は [theme-format.md](theme-format.md)。このガイドは「作る」側だけを扱う。

## 30秒でわかる流れ

```sh
cp -r themes/sample themes/mytheme        # サンプルをコピー
$EDITOR themes/mytheme/theme.json         # 色・角丸・画像を書く
python3 tools/theme_build.py themes/mytheme --preview
```

出力:

| 出力先 | 用途 |
|---|---|
| `sim/screenshots/theme_mytheme/90_theme_custom_home.png` | Home の確認 |
| `sim/screenshots/theme_mytheme/91_theme_custom_settings.png` | 設定画面の確認 |
| `sim/themes/mytheme.zip` | sim が読む |
| `tools/themes/mytheme.zip` | 配布用 |
| `android/app/src/main/assets/themes/mytheme.zip` | アプリ同梱 + 書き込み用 |

実機へは **スマホのアプリ → テーマ → zip を選ぶ → 送信**。
`--preview` は sim の安全域監査も走らせ、違反があれば失敗する (監査 0 が合格)。

## theme.json の形

```json
{
  "id": "mytheme",
  "name": "マイテーマ",
  "version": 1,
  "tokens": {
    "bg": "0x05070C",
    "surface": "0x111826",
    "primary": "0x38D6C4",
    "text": "0xEAF2F6",
    "radius_lg": 20,
    "anim_ms": 160,
    "font_body": 20,
    "font_digits": 96
  },
  "style": { "card_opa": 170, "border_w": 1, "border": "0x2E4560" },
  "screens": { "*": { "scrim": 40 }, "settings": { "scrim": 90 } }
}
```

- `id` は必須。`[a-z0-9-]{1,31}`。ファイル名にもなる
- 未知のキーは **エラー** (typo を黙って無視しない)
- 色は `"0xRRGGBB"` か `"#RRGGBB"`
- 画像は **theme.json と同じディレクトリのファイル名** で書く (下記)
- 全キーの一覧と意味は [theme-format.md](theme-format.md)

## 画像

```json
"images": { "home_bg": "bg.png", "face_chara": "chara.png" },
"screens": { "timer": { "bg": "timer_bg.png", "scrim": 60 } },
"icons": { "timer": "ic_timer.png" },
"assets": { "deco.star": "star.png" }
```

- 形式は PNG (8bit) か LVGL バイナリ (`.bin`)。ビルダーは変換せず zip に入れる
- 上限: `home_bg` 410x502 / `stand` 240x360 / `timer_done` 410x320 /
  `face_chara` 240x410 / 画面背景 410x502 / アイコン 12 件 / deco 4 枚/画面
- zip のエントリは 32 個まで、パッケージは 4 MiB まで
- 透過は PNG の α をそのまま使う (RGB565A8 に変換される)

## トークン

| 種別 | キー |
|---|---|
| 色 (19) | bg surface surface2 line primary primary2 on_primary text text_dim accent danger ok accent2 accent3 accent4 accent5 bubble_bg bubble_text edge |
| 数値 | radius_sm radius_lg (0..255) / space (0..64) / anim_ms (0..2000) |
| フォント | font_body font_title font_digits font_digits_sm = 20 / 26 / 56 / 96 |

`style` は card_radius card_opa border_w border glow_color glow_w btn_radius header。
`screens` は画面ごとの `bg` / `scrim` / `tokens` (色上書き) / `deco`。
画面名は `home quick more timer stopwatch counter steps memo settings ota
powermenu alarm notifications media agent alert` と `"*"`。

## スキン・フォント・キャラ

- **スキン** (9-slice のボタン/カード画像): `themes/<id>/kit.html` を書いて
  `python3 tools/build_themes.py --kit <id>`。パーツ PNG を個別に作ったなら
  `skin` に直接ファイル名を書き、`theme_build.py` で zip にできる
- **フォント**: `python3 tools/gen_fonts.py --binfont <ttf> --size 96` →
  `"fonts": {"digits": "digits.bin"}`
- **マスコット / 文字盤**: `mascot` / `face_layout` (書式は theme-format.md)
- **palette**: 名前付き色。`face_layout` の `color` で `"@pink"` と参照できる

## プレビューと検証

```sh
python3 tools/theme_build.py themes/mytheme --check     # 検証だけ
python3 tools/theme_build.py themes/mytheme --preview   # ビルド + 撮影
```

`--check` が通っても、見た目は `--preview` の PNG で確認する。
重なり・欠けグリフ・安全域は sim の監査が `audit 0 lines` で判定する。

### よくあるエラー

| メッセージ | 直し方 |
|---|---|
| `未知のキー 'xxx'` | キー名の typo。theme-format.md の表と照合 |
| `色 '...' が不正` | `0xRRGGBB` の 6 桁にする |
| `tokens.font_body: 20/26/56/96` | 内蔵フォントのサイズだけ使える |
| `ファイル 'x.png' がない` | theme.json と同じディレクトリに置く |
| `zip のエントリが 32 個を超える` | 画像を減らす / 1 枚にまとめる |
| 実機で `theme: apply 'x' -> standard` | 読み込み失敗。エントリ名・サイズ・manifest を確認 |
| 監査が 0 でない | 撮影された PNG を見て、はみ出し/重なりを直す |

## エージェント / LLM に書かせる場合

そのまま渡せる契約:

- 出力は `theme.json` のみ (コメント・説明文を混ぜない)
- 未知のキーを書かない。色は `"0xRRGGBB"`、画像は実在するファイル名
- 迷ったら `themes/sample/theme.json` を雛形にする
- 反復は `--check` → `--preview` → PNG を見る、の順

プロンプト例:

```
SAS Watch のテーマを theme.json で作って。条件:
- 夜の海 (深い紺の背景、ターコイズのアクセント、白い文字)
- radius_lg 22、カードは半透明 (card_opa 170)、枠線 1px
- 画像は使わない
出力は JSON のみ。キーは themes/sample/theme.json と同じ。
```

エージェントは `theme_build.py --preview` まで実行し、生成された
`90_..._home.png` / `91_..._settings.png` を確認してから次を直す。

## 既存テーマとの関係

- `mame` / `cosmos` / `cyber` / `cute` は従来の `tools/build_themes.py` で
  生成している (プログラム描画 / kit.html)。`theme.json` 経路と併用できる
- スタジオ (`tools/theme-studio/index.html`) は色・画像・画面背景を GUI で
  編集し、zip と **theme.json** を入出力できる。style / mascot / skin など
  スタジオ未対応のキーは zip 書き出しで失われるので、その場合は
  `theme_build.py` を使う
