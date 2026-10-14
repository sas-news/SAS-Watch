# Home と文字盤 (watch face) の整理案 — T22 調査・提案 (実装しない)

## 現状

### 2 つの「時計の顔」が重なっている

- **Home route** (`ui::kHomeScreen`): 実体は薄い委譲層。
  `faces/face_*.cpp` の `Ops` に build/on_event を丸投げするだけ
  (`screens/home.cpp` = 20行)。つまり **Home = 選択中の文字盤そのもの** で、
  別の「ホーム画面」コンテンツは存在しない。
- **文字盤 (faces/)**: `settings.face` (id 文字列) で選択。
  `faces.cpp::kFaces[]` に 7 種: bold / analog / hud / minimal /
  chara_side / chara_bubble / **theme** (manifest `face_layout` 駆動)。
  `face::current()` が設定を読んで Ops を返す。設定画面の文字盤一覧は
  `face::all()` + `label_ja()`。

### テーマ画像と文字盤の関係が散っている

| スロット | 使う場所 | 状態 |
|---|---|---|
| `images.home_bg` | **どの画面も参照していない** | **死にスロット** (読み込みだけ走る) |
| `images.stand` | chara_side/chara_bubble/theme のフォールバック | 実質「キャラ画像」 |
| `images.face_chara` | chara_* 3 面 + face_layout.chara | 文字盤固有なのにグローバル枠 |
| `images.timer_done` | shell.cpp のタイマーアラート | グローバルで妥当 |
| `face_layout` | `theme` 文字盤のみ | 文字盤設定なのにテーマ内に埋め込み |
| `screens.home.bg` | Home 画面背景 | こちらは生きている |

### 設定も 2 箇所

- `settings.face` — 文字盤 id (core の Settings)
- `settings.clock_font` — 時計フォント family ("auto" / 5 系統)、
  `kAutoFont[]` で文字盤ごとの見本フォントに対応

## 問題点

1. **「Home は文字盤」という関係が暗黙**: Route::Home の ScreenOps が
   `face::current()` に委譲する作りなので、Home の中身を増やす
   (通知数・次のアラーム等) と face と衝突する。
2. **home_bg が死んでいる**: テーマ作者が用意しても描かれない
   (screens.home.bg の方が使われる)。ドキュメントと実態がズレている。
3. **chara 系は画像前提の文字盤が global 枠に混ざっている**:
   `stand`/`face_chara` はキャラ盤でしか使わないのに `images` の
   固定スロットを占め、テーマ manifest 仕様を汚している。
4. **face_layout は「テーマの資産」なのに「1 文字盤の設定」**:
   `theme` という特別な face id 経由でしか効かず、v5 の assets と
   座標系が別管理。

## 提案: Home = 選択中の文字盤、固定 images 枠は標準盤の資産へ

### A. 構造の整理 (用語と責任の固定)

- **Home route** = 「選択中の文字盤を表示する画面」に限定する
  (現在どおり薄い委譲)。通知一覧・次アラーム等の「ホーム情報」は
  Home に追加せず、必要なら quick/通知画面側に置く。
- **文字盤 = face**: `faces/` の Ops を all()/find()/current() で引く
  現在の形を維持。新しい文字盤は Ops を1つ足すだけ (T19 的発想を
  face にも適用できる余地は残すが今回は提案しない)。

### B. images スロットの再配置

- `home_bg` → **廃止** (どこからも描かれていない)。
  同等機能は `assets."home_bg"` を標準盤 (bold 等) が
  `theme_image("home_bg")` で引く形に置き換える。
- `stand` / `face_chara` → **chara 系盤の資産**として扱いを明記し、
  スロット名は維持しつつ「`theme_image` の id として引ける別名」と
  再解釈する (v5 assets で既に id として引けるため、単にドキュメント化)。
- `timer_done` はグローバル (アラート装飾) のまま維持。
- 新しい「盤専用資産」は `assets."face.<face_id>.<name>"` 規約にすると
  拡張が破綻しない (例: `face.bold.bg`)。

### C. face_layout と assets の統合

- `face_layout` の `chara.img` などの画像参照は、ファイル名直指定から
  `assets` の id 参照 (`"img": "@deco.star"` 等) に移すことを検討
  (パース時に assets 表の存在検証ができる)。
- `theme` 盤は `face_layout` を持つテーマでのみ出すという制約は残す。

### D. 移行ステップ (実装する場合)

1. `img_home_bg` の描画参照が無いことを再確認してスロット自体を削除
   (manifest の images キー名は残して assets 別名として解釈)。
2. chara_* 盤の `t.img_face_chara / t.img_stand` を
   `theme_image("face_chara")` / `theme_image("stand")` に置き換える
   (動作は同じ・遅延ロードになる)。
3. `face.<id>.` 規約を theme-format.md に書く。
4. `face_layout` の img を assets 参照にするかは別判断
   (現状のファイル名直指定でも動くので後回しでよい)。

## 判断してほしい点

- **B**: `home_bg` を消すか、「標準盤の背面帯」として再実装するか。
- **D-4**: face_layout の img を assets id 参照に変えるか
  (zip 検証が効く代わりに既存テーマの互換処理が要る)。
