// components.hpp — Theme から作る共通部品。
// 全てのインタラクティブ要素に EVENT_BUBBLE を付ける
// (画面ルートまで GESTURE / LONG_PRESSED が届くようにするため)。
#pragma once

#include "theme.hpp"
#include "ui/ui.hpp"

namespace ui::c {

// 画面直上のヘッダ (高さ 76px)。back_btn=true で左に「‹ 戻る」ピル。
lv_obj_t* header(lv_obj_t* scr, const char* title, bool back_btn);

// 縦積み・スクロール可のコンテンツ列 (左右 32px)。ヘッダの下に敷く。
lv_obj_t* content(lv_obj_t* scr);

// スクロール内容のスナップショット化 (押しっぱスクロール軽量化)。
// col の子 inner (内容全高のコンテナ) を RGB565 の1枚の画像に焼き直し、
// zones[] の矩形に透明クリックゾーンを復元する。スクロール中の再描画が
// 全子要素の再描画から画像ブリットに変わり大幅に軽くなる。
// ※ 静的リスト専用。動的更新・スイッチ等の状態付き部品は焼き込めない。
struct BakeZone {
  lv_coord_t x;
  lv_coord_t y;
  lv_coord_t w;
  lv_coord_t h;
  lv_event_cb_t cb;
  void* ud;
};
// row の矩形を col 基準の BakeZone に変換 (レイアウト確定後に呼ぶ)。
BakeZone bake_rect(lv_obj_t* col, lv_obj_t* row, lv_event_cb_t cb,
                   void* ud);
// inner を画像に置き換え、zones のクリックゾーンを col 内に作る。
// スナップショット失敗時は false で何もしない。
bool bake_scroll(lv_obj_t* col, lv_obj_t* inner, const BakeZone* zones,
                 size_t zone_cnt);

// フル幅ピルボタン (高さ 58px)。cb は LV_EVENT_CLICKED。
lv_obj_t* button(lv_obj_t* parent, const char* text, lv_event_cb_t cb,
                 void* user_data);
// プライマリ色のボタン。
lv_obj_t* button_primary(lv_obj_t* parent, const char* text, lv_event_cb_t cb,
                        void* user_data);
// 危険色 (削除など)。danger の薄い塗り + danger 文字。
lv_obj_t* button_danger(lv_obj_t* parent, const char* text, lv_event_cb_t cb,
                       void* user_data);

// 破壊的な操作の確認ダイアログ (T14 操作ルール)。
// 全画面の半透明オーバーレイ + 中央カード (msg + 「はい」「いいえ」)。
// 「はい」で cb を呼んで閉じる。「いいえ」・枠外タップはただ閉じる。
// 同時に1枚。cb/ud は yes ボタンの CLICKED にそのまま渡す。
void confirm(lv_obj_t* parent, const char* msg, lv_event_cb_t cb, void* ud);

// グループ: 行を 1px 区切り線つきで縦に並べるカード (pad 0)。
lv_obj_t* group(lv_obj_t* parent);

// パネル: 自由レイアウト用のカード (pad 12)。子は縦積み。
lv_obj_t* card(lv_obj_t* parent);

// 行の土台だけ作る (中身は自分で組む)。区切り線もここで入れる。
lv_obj_t* row_box(lv_obj_t* grp, lv_event_cb_t cb, void* ud);

// row_box 内に置く「タイトル (+ 下に小さい sub)」の縦セル。幅いっぱいに伸びる。
// max_w = タイトル/sub の最大幅 (超えた分は DOT で省略)。
lv_obj_t* row_text(lv_obj_t* row, const char* text, const char* sub,
                   int32_t max_w);

// 右側セル用ヘルパ: 値テキスト (text_dim)。
lv_obj_t* row_value(lv_obj_t* row, const char* value);

// 行 (text + 任意 sub + 右の値 or ›)。grp は group() の中に。
lv_obj_t* row(lv_obj_t* grp, const char* text, const char* sub,
              const char* value, bool chevron, lv_event_cb_t cb, void* ud);

// 「‹ 戻る」行 (掘り下げビューの先頭に置く)。
lv_obj_t* back_row(lv_obj_t* grp, lv_event_cb_t cb);

// 先頭に色タイル (icon=1-2文字) を置く行。
lv_obj_t* row_icon(lv_obj_t* grp, const char* icon, lv_color_t icon_bg,
                   const char* text, const char* sub, bool chevron,
                   lv_event_cb_t cb, void* ud);

// row_icon の画像版 (テーマ icons)。imgd==nullptr なら文字タイル。
lv_obj_t* row_icon_img(lv_obj_t* grp, const lv_image_dsc_t* imgd,
                       const char* icon, lv_color_t icon_bg,
                       const char* text, const char* sub, bool chevron,
                       lv_event_cb_t cb, void* ud);

// 画面隅のマスコット (manifest mascot。screen_index は kThemeScreenNames 順)。
// 有効画面でなければ nullptr。タップで表情とセリフが切り替わる。
lv_obj_t* mascot(lv_obj_t* scr, int screen_index);

// 右にスイッチを置く行 (行タップで cb。ON/OFF の見た目はスイッチ任せ)。
lv_obj_t* row_switch(lv_obj_t* grp, const char* text, const char* sub,
                     bool on, lv_event_cb_t cb, void* ud);

// 行の右端に置くスイッチ部品 (非クリック: 行タップでトグル)。
lv_obj_t* mk_switch(lv_obj_t* row, bool on);

// row_switch / mk_switch で置いたスイッチを行から取る。
lv_obj_t* switch_of(lv_obj_t* row);

// グループの上に置く小キャプション (「画面」「音と振動」など)。
lv_obj_t* caption(lv_obj_t* parent, const char* text);

// ラベル + 値 + トラックのスライダーブロック。grp 内なら行として、
// content 直下なら自分のグループを作る。VALUE_CHANGED で cb。
lv_obj_t* slider_row(lv_obj_t* parent, const char* label, const char* suffix,
                     int32_t min, int32_t max, int32_t value,
                     lv_event_cb_t cb, void* ud);
// slider_row が返すコンテナから実スライダーを取る。
lv_obj_t* slider_of(lv_obj_t* row);

// 外部から値を書き換える (LV_ANIM_OFF)。set_value は VALUE_CHANGED を投げないので
// 値ラベルもここで更新する。
void slider_set(lv_obj_t* row, int32_t value);

// 小さい補足行。
lv_obj_t* line(lv_obj_t* parent, const char* text);

// 状態ドット (丸)。接続/未接続など。
lv_obj_t* status_dot(lv_obj_t* parent, lv_color_t col);

// ドットの色を変える。
void set_dot(lv_obj_t* dot, lv_color_t col);

// v3 skin: obj に part の画像スキンを貼る (9-slice)。画像があれば
// ベクタ背景を消して true、なければ何もせず false。face など
// 共通コンポーネント外のオブジェクトにも使える。
bool skin_obj(lv_obj_t* o, watch::ThemeSkinPartId part);

}  // namespace ui::c
