// components.cpp — 共通部品の実装。
//   見た目は docs/design/ui.html のモックアップに合わせる:
//   - 黒背景 + 角丸グループ (surface) に行を区切り線で並べる
//   - 左右 22px、ヘッダ 76px、ボタンは高 58px のピル
//   - 色・角丸・フォントは Theme トークンから取る (file テーマも同じ形)。
#include "components.hpp"

#include "ui/port.hpp"

// ‹ › (U+2039/203A) だけの小さなフォント。font_jp_* は既定収録範囲外で
// 再生成すると既存グリフが全部入れ替わるので、シェブロン専用に分けた。
LV_FONT_DECLARE(font_chev_22);

namespace ui::c {

namespace {

constexpr lv_coord_t kW = 410;     // 表示幅
constexpr lv_coord_t kPad = 22;    // コンテンツの左右余白 (角丸 44px の内側)
constexpr lv_coord_t kHeaderH = 76;
constexpr lv_coord_t kBackH = 48;  // 戻るピル高さ
constexpr lv_coord_t kRowMinH = 60;
constexpr lv_coord_t kRowPadX = 16;
constexpr lv_coord_t kRowGap = 14;
constexpr lv_coord_t kIconTile = 36;
constexpr lv_coord_t kBtnH = 58;
constexpr lv_coord_t kSwitchW = 52;
constexpr lv_coord_t kSwitchH = 30;
constexpr lv_coord_t kTrackH = 10;
constexpr lv_coord_t kKnob = 24;

// group() で作ったオブジェクトを区別するマーカー (slider_row が
// 「グループ内のブロック」か「自分のグループ」かを決めるのに使う)。
void* const kGroupTag = const_cast<char*>("ui::c::group");

// CLICKED は画面ルートまでバブルするので、ルートの cb 1つで
// 全ボタン/行タップのクリック音を拾える。
void on_any_click(lv_event_t*) { ui::port::click(); }

void bubble(lv_obj_t* o) { lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE); }

void clickable(lv_obj_t* o) {
  lv_obj_add_flag(
      o, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE |
                                    LV_OBJ_FLAG_EVENT_BUBBLE));
}

// フラットな子コンテナ (枠なし・透過・スクロール無し)。
lv_obj_t* flat(lv_obj_t* parent) {
  lv_obj_t* o = lv_obj_create(parent);
  bubble(o);
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

lv_obj_t* mk_label(lv_obj_t* parent, const char* text,
                   const lv_font_t* font, lv_color_t col) {
  lv_obj_t* l = lv_label_create(parent);
  bubble(l);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, col, 0);
  return l;
}

// グループ内でアイテム (行・スライダーブロック) の前に区切り線を挿す。
void divider_if_needed(lv_obj_t* grp) {
  const Theme& t = theme();
  if (lv_obj_get_child_count(grp) == 0) return;
  lv_obj_t* d = flat(grp);
  lv_obj_set_size(d, LV_PCT(100), 1);
  lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(d, t.line, 0);
}

// 右端の › (モックの .ch #4A4F5C)。専用フォントで描く。
lv_obj_t* chevron(lv_obj_t* row) {
  const Theme& t = theme();
  lv_obj_t* ch = mk_label(row, "›", &font_chev_22, t.edge);
  return ch;
}

}  // namespace

lv_obj_t* header(lv_obj_t* scr, const char* title, bool back_btn) {
  const Theme& t = theme();
  lv_obj_add_event_cb(scr, on_any_click, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* h = flat(scr);
  lv_obj_set_size(h, kW, kHeaderH);
  lv_obj_set_pos(h, 0, 0);
  lv_obj_set_style_pad_left(h, kPad, 0);
  lv_obj_set_style_pad_right(h, kPad, 0);
  // コンテンツ領域の中央がピル中心 ~41px になる pad (モック準拠)。
  lv_obj_set_style_pad_top(h, 18, 0);
  lv_obj_set_style_pad_bottom(h, 12, 0);
  lv_obj_set_style_pad_column(h, 12, 0);
  lv_obj_set_flex_flow(h, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(h, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  if (back_btn) {
    lv_obj_t* b = lv_button_create(h);
    clickable(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, kBackH);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, t.surface2, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_left(b, 12, 0);
    lv_obj_set_style_pad_right(b, 16, 0);
    lv_obj_set_style_pad_column(b, 4, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    mk_label(b, "‹", &font_chev_22, t.primary);
    mk_label(b, "戻る", t.font_body, t.text_dim);
    lv_obj_add_event_cb(
        b, [](lv_event_t*) { ui::emit(watch::ActionType::Back); },
        LV_EVENT_CLICKED, nullptr);
  }

  lv_obj_t* tl = mk_label(h, title, t.font_title, t.text);
  (void)tl;
  return h;
}

lv_obj_t* content(lv_obj_t* scr) {
  const Theme& t = theme();
  lv_obj_t* c = flat(scr);
  lv_obj_set_size(c, kW - 2 * kPad, 502 - kHeaderH);
  lv_obj_set_pos(c, kPad, kHeaderH);
  lv_obj_set_style_pad_row(c, 12, 0);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_scroll_dir(c, LV_DIR_VER);
  lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  // スクロールバーは出さない (腕時計 UI、カードの上に太く被さるため)。
  lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_OFF);
  (void)t;
  return c;
}

namespace {

lv_obj_t* mk_button(lv_obj_t* parent, const char* text,
                    lv_event_cb_t cb, void* ud, lv_color_t bg,
                    lv_opa_t bg_opa, lv_color_t fg) {
  const Theme& t = theme();
  lv_obj_t* b = lv_button_create(parent);
  clickable(b);
  lv_obj_set_size(b, LV_PCT(100), kBtnH);
  lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(b, bg, 0);
  lv_obj_set_style_bg_opa(b, bg_opa, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_t* l = mk_label(b, text, t.font_body, fg);
  lv_obj_center(l);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  return b;
}

}  // namespace

lv_obj_t* button(lv_obj_t* parent, const char* text, lv_event_cb_t cb,
                 void* ud) {
  const Theme& t = theme();
  return mk_button(parent, text, cb, ud, t.surface2, LV_OPA_COVER, t.text);
}

lv_obj_t* button_primary(lv_obj_t* parent, const char* text, lv_event_cb_t cb,
                        void* ud) {
  const Theme& t = theme();
  return mk_button(parent, text, cb, ud, t.primary, LV_OPA_COVER,
                   t.on_primary);
}

lv_obj_t* button_danger(lv_obj_t* parent, const char* text, lv_event_cb_t cb,
                       void* ud) {
  const Theme& t = theme();
  // モックの rgba(danger, .16)。
  return mk_button(parent, text, cb, ud, t.danger, 41, t.danger);
}

lv_obj_t* group(lv_obj_t* parent) {
  const Theme& t = theme();
  lv_obj_t* g = lv_obj_create(parent);
  bubble(g);
  lv_obj_set_size(g, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_radius(g, t.radius_lg, 0);
  lv_obj_set_style_bg_color(g, t.surface, 0);
  lv_obj_set_style_border_width(g, 0, 0);
  lv_obj_set_style_pad_all(g, 0, 0);
  lv_obj_set_style_pad_row(g, 0, 0);  // pad_all では行間はゼロにならない
  lv_obj_set_flex_flow(g, LV_FLEX_FLOW_COLUMN);
  lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_user_data(g, kGroupTag);
  return g;
}

lv_obj_t* card(lv_obj_t* parent) {
  const Theme& t = theme();
  lv_obj_t* c = lv_obj_create(parent);
  bubble(c);
  lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_radius(c, t.radius_lg, 0);
  lv_obj_set_style_bg_color(c, t.surface, 0);
  lv_obj_set_style_border_width(c, 0, 0);
  lv_obj_set_style_pad_all(c, 12, 0);
  lv_obj_set_style_pad_row(c, t.space, 0);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_START);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  return c;
}

lv_obj_t* row_box(lv_obj_t* grp, lv_event_cb_t cb, void* ud) {
  divider_if_needed(grp);
  lv_obj_t* r = flat(grp);
  if (cb) clickable(r);
  lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_min_height(r, kRowMinH, 0);
  lv_obj_set_style_pad_left(r, kRowPadX, 0);
  lv_obj_set_style_pad_right(r, kRowPadX, 0);
  lv_obj_set_style_pad_column(r, kRowGap, 0);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  if (cb) lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, ud);
  return r;
}

lv_obj_t* row_text(lv_obj_t* row, const char* text, const char* sub,
                   int32_t max_w) {
  const Theme& t = theme();
  lv_obj_t* cell = flat(row);
  lv_obj_set_size(cell, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_min_width(cell, 60, 0);  // flex_grow と組で右要素を右端へ
  // font_jp_20 は line_height 38 でグリフ (20px) より箱が大きいので、
  // 負の行間でサブ行をタイトル直下に寄せる (視覚的に ~2-4px)。
  lv_obj_set_style_pad_row(cell, -14, 0);
  // 行が SIZE_CONTENT だと LVGL は track_place を START に固定し、
  // 子が行の上寄りに張り付く。セルに行の最低高さを持たせて、セル内で
  // タイトル (+サブ) ブロックを垂直中央揃えにする。
  lv_obj_set_style_min_height(cell, kRowMinH, 0);
  lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_flex_grow(cell, 1);

  // PCT(100) はセル自体が SIZE_CONTENT のので測定時に潰れて縦に巻く。
  // 行内の残り幅を呼び側から計算して固定幅 + DOT にする。
  lv_obj_t* l = mk_label(cell, text, t.font_body, t.text);
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  lv_obj_set_width(l, max_w);
  if (sub) {
    lv_obj_t* s = mk_label(cell, sub, t.font_body, t.text_dim);
    lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s, max_w);
  }
  return cell;
}

namespace {

// 行内容幅 = 366 - 左右pad 16*2。タイトルセルの残り予算を引く。
int32_t text_budget(bool icon, bool chev, bool value) {
  int32_t w = 334;
  if (icon) w -= kIconTile + kRowGap;
  if (chev) w -= 22 + kRowGap;
  if (value) w -= 100;  // 値テキストの予約分 (短い値想定)
  return w < 60 ? 60 : w;
}

}  // namespace

lv_obj_t* row_value(lv_obj_t* row, const char* value) {
  const Theme& t = theme();
  return mk_label(row, value, t.font_body, t.text_dim);
}

lv_obj_t* row(lv_obj_t* grp, const char* text, const char* sub,
              const char* value, bool chev, lv_event_cb_t cb, void* ud) {
  lv_obj_t* r = row_box(grp, cb, ud);
  row_text(r, text, sub, text_budget(false, chev, value != nullptr));
  if (value) row_value(r, value);
  if (chev) chevron(r);
  return r;
}

lv_obj_t* back_row(lv_obj_t* grp, lv_event_cb_t cb) {
  const Theme& t = theme();
  lv_obj_t* r = row_box(grp, cb, nullptr);
  mk_label(r, "‹", &font_chev_22, t.primary);
  row_text(r, "戻る", nullptr, text_budget(false, false, false));
  return r;
}

lv_obj_t* row_icon(lv_obj_t* grp, const char* icon, lv_color_t icon_bg,
                   const char* text, const char* sub, bool chev,
                   lv_event_cb_t cb, void* ud) {
  const Theme& t = theme();
  lv_obj_t* r = row_box(grp, cb, ud);
  if (icon) {
    lv_obj_t* ic = flat(r);
    lv_obj_set_size(ic, kIconTile, kIconTile);
    lv_obj_set_style_radius(ic, 11, 0);
    lv_obj_set_style_bg_opa(ic, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(ic, icon_bg, 0);
    lv_obj_t* il = mk_label(ic, icon, t.font_body, lv_color_hex(0x000000));
    lv_obj_center(il);
  }
  row_text(r, text, sub, text_budget(icon != nullptr, chev, false));
  if (chev) chevron(r);
  return r;
}

lv_obj_t* mk_switch(lv_obj_t* row, bool on) {
  const Theme& t = theme();
  lv_obj_t* sw = lv_switch_create(row);
  // スイッチ自身はクリックを取らない (行タップでトグル → 二重発火防止)。
  bubble(sw);
  lv_obj_remove_flag(sw, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(sw, kSwitchW, kSwitchH);
  lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  // OFF: トラック≈#3A3E4A (edge×line)、ノブ≈#AAB8BB。ON: 塗り primary + 白ノブ。
  // LVGL 既定テーマは CHECKED の INDICATOR を青にするので同じセレクタで上書き
  // (汎用セレクタだけだと ON が青のまま・OFF のノブ脇に橙が滲む)。
  lv_obj_set_style_bg_color(sw, lv_color_mix(t.edge, t.line, 128),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
  // OFF 時は INDICATOR もトラック色にして左端の滲みを消す (LVGL は
  // OFF でもインジケータを少し描く)。ON だけ CHECKED セレクタで primary。
  lv_obj_set_style_bg_color(sw, lv_color_mix(t.edge, t.line, 128),
                            LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sw, t.primary,
                            LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_bg_opa(sw, LV_OPA_COVER,
                          LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(
      sw, lv_color_mix(t.text_dim, t.text, 170), LV_PART_KNOB);
  lv_obj_set_style_bg_color(sw, lv_color_hex(0xFFFFFF),
                            LV_PART_KNOB | LV_STATE_CHECKED);
  lv_obj_set_style_pad_all(sw, (kSwitchH - kKnob) / 2, LV_PART_KNOB);
  if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
  return sw;
}

lv_obj_t* row_switch(lv_obj_t* grp, const char* text, const char* sub,
                     bool on, lv_event_cb_t cb, void* ud) {
  lv_obj_t* r = row_box(grp, cb, ud);
  row_text(r, text, sub, text_budget(false, false, true));
  mk_switch(r, on);
  return r;
}

lv_obj_t* switch_of(lv_obj_t* row) {
  // 最後の子がスイッチ (row_text のセルと仕切りを除く)。
  const uint32_t n = lv_obj_get_child_count(row);
  return n ? lv_obj_get_child(row, static_cast<int32_t>(n) - 1) : nullptr;
}

lv_obj_t* caption(lv_obj_t* parent, const char* text) {
  const Theme& t = theme();
  lv_obj_t* l = mk_label(parent, text, t.font_body, t.text_dim);
  // content 列は子を水平センタリングするので、幅いっぱいにして左揃え。
  lv_obj_set_width(l, LV_PCT(100));
  lv_obj_set_style_text_letter_space(l, 2, 0);
  lv_obj_set_style_pad_left(l, 6, 0);
  lv_obj_set_style_pad_bottom(l, 0, 0);
  return l;
}

namespace {

// 値表示の更新 (スライダーの VALUE_CHANGED で先に呼ぶ)。user_data = 値ラベル。
void slider_upd_label(lv_obj_t* s, lv_obj_t* vl) {
  const char* suf =
      static_cast<const char*>(lv_obj_get_user_data(s));
  lv_label_set_text_fmt(vl, "%d%s", static_cast<int>(lv_slider_get_value(s)),
                        suf ? suf : "");
}

void on_slider_value(lv_event_t* e) {
  slider_upd_label(lv_event_get_target_obj(e),
                   static_cast<lv_obj_t*>(lv_event_get_user_data(e)));
}

// スライダー塗りのグラデーション (primary → primary2)。style が参照するので静的。
lv_grad_dsc_t s_sl_grad;
bool s_sl_grad_init = false;

lv_obj_t* slider_inner(lv_obj_t* grp, const char* label, const char* suffix,
                       int32_t min, int32_t max, int32_t value,
                       lv_event_cb_t cb, void* ud) {
  const Theme& t = theme();
  divider_if_needed(grp);
  lv_obj_t* inner = flat(grp);
  lv_obj_set_size(inner, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_pad_top(inner, 14, 0);
  lv_obj_set_style_pad_bottom(inner, 16, 0);
  lv_obj_set_style_pad_left(inner, kRowPadX, 0);
  lv_obj_set_style_pad_right(inner, kRowPadX, 0);
  lv_obj_set_style_pad_row(inner, 12, 0);
  lv_obj_set_flex_flow(inner, LV_FLEX_FLOW_COLUMN);

  lv_obj_t* top = flat(inner);
  lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  mk_label(top, label, t.font_body, t.text);
  lv_obj_t* vl = mk_label(top, "", t.font_body, t.primary);

  lv_obj_t* s = lv_slider_create(inner);
  clickable(s);
  lv_obj_set_width(s, LV_PCT(100));
  lv_obj_set_height(s, kTrackH);
  lv_slider_set_range(s, min, max);
  lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s, t.line, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
  // 塗りは primary→primary2 の横グラデーション (初回だけ作る)。
  if (!s_sl_grad_init) {
    const lv_color_t cols[2] = {t.primary, t.primary2};
    const lv_opa_t opas[2] = {LV_OPA_COVER, LV_OPA_COVER};
    const uint8_t fr[2] = {0, 255};
    lv_grad_init_stops(&s_sl_grad, cols, opas, fr, 2);
    lv_grad_linear_init(&s_sl_grad, LV_GRAD_LEFT, LV_GRAD_CENTER,
                        LV_GRAD_RIGHT, LV_GRAD_CENTER, LV_GRAD_EXTEND_PAD);
    s_sl_grad_init = true;
  }
  lv_obj_set_style_bg_grad(s, &s_sl_grad, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s, t.primary, LV_PART_INDICATOR);
  // 白いノブ + primary の外周リング (モックの box-shadow 相当)。
  lv_obj_set_style_bg_color(s, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
  lv_obj_set_style_pad_all(s, (kKnob - kTrackH) / 2, LV_PART_KNOB);
  lv_obj_set_style_outline_color(s, t.primary, LV_PART_KNOB);
  lv_obj_set_style_outline_opa(s, 90, LV_PART_KNOB);
  lv_obj_set_style_outline_width(s, 4, LV_PART_KNOB);
  lv_obj_set_user_data(s, const_cast<char*>(suffix));
  // 値ラベル更新 → ユーザの cb の順で呼ばれる。
  // 注意: LV_ANIM_OFF の set_value は VALUE_CHANGED を投げない (ドラッグ時のみ)
  // ので初期値はここで直接ラベルに書く。
  lv_obj_add_event_cb(s, on_slider_value, LV_EVENT_VALUE_CHANGED, vl);
  lv_slider_set_value(s, value, LV_ANIM_OFF);
  slider_upd_label(s, vl);
  if (cb) lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, ud);
  return inner;
}

}  // namespace

lv_obj_t* slider_row(lv_obj_t* parent, const char* label, const char* suffix,
                     int32_t min, int32_t max, int32_t value,
                     lv_event_cb_t cb, void* ud) {
  // 親が group() ならその中にブロックを入れる。違うならグループを作る。
  lv_obj_t* grp = (lv_obj_get_user_data(parent) == kGroupTag)
                      ? parent
                      : group(parent);
  return slider_inner(grp, label, suffix, min, max, value, cb, ud);
}

lv_obj_t* slider_of(lv_obj_t* row) {
  // slider_row が返す inner の子: 0=上段 (ラベル+値), 1=スライダー。
  return lv_obj_get_child(row, 1);
}

namespace {

lv_obj_t* slider_value_label(lv_obj_t* row) {
  // inner の子 0=上段。その子 0=ラベル, 1=値。
  lv_obj_t* top = lv_obj_get_child(row, 0);
  return top ? lv_obj_get_child(top, 1) : nullptr;
}

}  // namespace

void slider_set(lv_obj_t* row, int32_t value) {
  lv_obj_t* s = slider_of(row);
  lv_slider_set_value(s, value, LV_ANIM_OFF);
  lv_obj_t* vl = slider_value_label(row);
  if (vl) slider_upd_label(s, vl);
}

lv_obj_t* line(lv_obj_t* parent, const char* text) {
  const Theme& t = theme();
  return mk_label(parent, text, t.font_body, t.text_dim);
}

lv_obj_t* status_dot(lv_obj_t* parent, lv_color_t col) {
  lv_obj_t* d = lv_obj_create(parent);
  bubble(d);
  lv_obj_set_size(d, 14, 14);
  lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(d, col, 0);
  lv_obj_set_style_border_width(d, 0, 0);
  lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE);
  return d;
}

void set_dot(lv_obj_t* dot, lv_color_t col) {
  lv_obj_set_style_bg_color(dot, col, 0);
}

}  // namespace ui::c
