// components.cpp — 共通部品の実装。
//   見た目は docs/design/ui.html のモックアップに合わせる:
//   - 黒背景 + 角丸グループ (surface) に行を区切り線で並べる
//   - 左右 22px、ヘッダ 76px、ボタンは高 58px のピル
//   - 色・角丸・フォントは Theme トークンから取る (file テーマも同じ形)。
#include "components.hpp"

#include "theme_res.hpp"
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

// header() で作ったヘッダのマーカー (mascot が小型表示か隅表示かを
// 判別するために使う)。
void* const kHeaderTag = const_cast<char*>("ui::c::header");

// mk_label() で作ったラベルのマーカー (skin の text/text_pressed で
// 色を差し替える対象の識別に使う)。
void* const kLabelTag = const_cast<char*>("ui::c::label");

// v3 スキン適用ヘルパ (このファイル後半の 9-slice 実装)。
// 画像があれば true。
bool skin_apply_obj(lv_obj_t* o, watch::ThemeSkinPartId part, int mode,
                    bool use_pad);

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
  // スキンの text/text_pressed で塗り替える対象としてマーク。
  lv_obj_set_user_data(l, kLabelTag);
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
  skin_apply_obj(d, watch::kSkinDivider, 0, false);
}

// 右端の › (モックの .ch #4A4F5C)。専用フォントで描く。
lv_obj_t* chevron(lv_obj_t* row) {
  const Theme& t = theme();
  lv_obj_t* ch = mk_label(row, "›", &font_chev_22, t.edge);
  return ch;
}

// ---- v2 style スキン (ThemeStyleSkin::set ビットが指定フラグ) ----

lv_coord_t skin_card_radius() {
  const Theme& t = theme();
  return (t.style.set & (1u << watch::kStyleCardRadius))
             ? t.style.card_radius
             : t.radius_lg;
}

lv_coord_t skin_btn_radius() {
  const Theme& t = theme();
  return (t.style.set & (1u << watch::kStyleBtnRadius))
             ? t.style.btn_radius
             : LV_RADIUS_CIRCLE;
}

// group/card の「カードっぽさ」をまとめて適用。
void skin_card(lv_obj_t* o) {
  const Theme& t = theme();
  lv_obj_set_style_radius(o, skin_card_radius(), 0);
  lv_obj_set_style_bg_color(o, t.surface, 0);
  lv_obj_set_style_bg_opa(
      o,
      (t.style.set & (1u << watch::kStyleCardOpa)) ? t.style.card_opa
                                                   : LV_OPA_COVER,
      0);
  const uint8_t bw = (t.style.set & (1u << watch::kStyleBorderW))
                         ? t.style.border_w
                         : 0;
  if (bw) {
    lv_obj_set_style_border_width(o, bw, 0);
    lv_obj_set_style_border_color(
        o,
        (t.style.set & (1u << watch::kStyleBorder))
            ? lv_color_hex(t.style.border)
            : t.line,
        0);
  }
  // glow: カードの影 (glow_w 指定時のみ)。
  if (t.style.set & (1u << watch::kStyleGlowW)) {
    lv_obj_set_style_shadow_width(o, t.style.glow_w, 0);
    lv_obj_set_style_shadow_color(
        o,
        (t.style.set & (1u << watch::kStyleGlow))
            ? lv_color_hex(t.style.glow)
            : t.primary,
        0);
    lv_obj_set_style_shadow_opa(o, 120, 0);
    lv_obj_set_style_shadow_spread(o, 0, 0);
  }
}

// ---- v3 画像スキン (manifest "skin" の 9-slice) ----

// LV_STATE → SkinSet::img の index。
int skin_state_idx(lv_state_t st) {
  if (st & LV_STATE_DISABLED) return 3;
  if (st & LV_STATE_CHECKED) return 2;
  if (st & LV_STATE_PRESSED) return 1;
  return 0;
}

// reg 画像を (x,y,w,h) に伸縮して描画。lv_draw_image は coords を
// 「拡大前の画像の置き場所」として解釈する (変換は pivot 起点) ので、
// coords には src サイズの矩形を渡す。
// scale は floor ではなく ceil を使う: LVGL は描画幅を
// (src*scale)>>8 の切り捨てで決めるため floor だと要求幅より最大1px
// 足りず、領域境に未描画の1px抜け線が出る。ceil による最大1pxの
// はみ出しは、後で描かれる隣接領域が上書きするので見えない。
void skin_draw_reg(lv_layer_t* layer, const lv_image_dsc_t* d,
                   int32_t x, int32_t y, int32_t w, int32_t h) {
  if (!d || !d->data || !d->header.w || !d->header.h || w <= 0 || h <= 0) {
    return;
  }
  lv_draw_image_dsc_t dsc;
  lv_draw_image_dsc_init(&dsc);
  dsc.src = d;
  dsc.scale_x = static_cast<int32_t>(
      (w * LV_SCALE_NONE + static_cast<int32_t>(d->header.w) - 1) /
      static_cast<int32_t>(d->header.w));
  dsc.scale_y = static_cast<int32_t>(
      (h * LV_SCALE_NONE + static_cast<int32_t>(d->header.h) - 1) /
      static_cast<int32_t>(d->header.h));
  dsc.pivot = {0, 0};
  lv_area_t a = {x, y, x + d->header.w - 1, y + d->header.h - 1};
  lv_draw_image(layer, &dsc, &a);
}

// SkinImg の 9 領域を area に配置して描画 (角=原寸、辺/中央=伸縮)。
void skin_draw_9(lv_layer_t* layer, const theme_res::SkinImg* im,
                 const lv_area_t* a) {
  int32_t l = im->slice[0];
  int32_t tp = im->slice[1];
  int32_t r = im->slice[2];
  int32_t b = im->slice[3];
  const int32_t w = lv_area_get_width(a);
  const int32_t h = lv_area_get_height(a);
  if (w <= 0 || h <= 0) return;
  // 対象が slice より小さいときは潰れないよう縮める。
  if (l + r > w) {
    l = w / 2;
    r = w - l;
  }
  if (tp + b > h) {
    tp = h / 2;
    b = h - tp;
  }
  const int32_t xs[4] = {a->x1, a->x1 + l, a->x2 - r + 1, a->x2 + 1};
  const int32_t ys[4] = {a->y1, a->y1 + tp, a->y2 - b + 1, a->y2 + 1};
  for (int i = 0; i < 9; ++i) {
    const lv_image_dsc_t* d = &im->reg[i];
    if (!d->data) continue;
    const int ry = i / 3;
    const int cx = i % 3;
    skin_draw_reg(layer, d, xs[cx], ys[ry], xs[cx + 1] - xs[cx],
                  ys[ry + 1] - ys[ry]);
  }
}

// event user_data = part | (mode << 8)。mode 0=全面 / 1=下端の帯。
void* skin_ud(int part, int mode) {
  return reinterpret_cast<void*>(
      static_cast<uintptr_t>(part | (mode << 8)));
}

// 汎用 DRAW_MAIN コールバック: 現在 state の part 画像を 9-slice で描く。
void skin_draw_cb(lv_event_t* e) {
  lv_obj_t* o = lv_event_get_target_obj(e);
  const uintptr_t ud =
      reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
  const int part = static_cast<int>(ud & 0xFF);
  const Theme& t = theme();
  const theme_res::SkinSet* ss = t.skin[part];
  if (!ss) return;
  const theme_res::SkinImg* im = ss->img[skin_state_idx(lv_obj_get_state(o))];
  if (!im) im = ss->img[0];
  if (!im) return;
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  if ((ud >> 8) == 1) {
    // mode 1: 下端の帯 (caption_line)。画像の高さぶんだけ下に張る。
    const int32_t s = im->reg[4].data
                          ? static_cast<int32_t>(im->reg[4].header.h)
                          : 4;
    a.y1 = a.y2 - (s - 1);
  }
  skin_draw_9(lv_event_get_layer(e), im, &a);
}

// ラベル (kLabelTag) の文字色を再帰で塗り替える。
void skin_recolor(lv_obj_t* o, lv_color_t col) {
  if (lv_obj_get_user_data(o) == kLabelTag) {
    lv_obj_set_style_text_color(o, col, 0);
    return;
  }
  const uint32_t n = lv_obj_get_child_count(o);
  for (uint32_t i = 0; i < n; ++i) {
    skin_recolor(lv_obj_get_child(o, static_cast<int32_t>(i)), col);
  }
}

void skin_recolor_part(lv_obj_t* o, int part, bool pressed) {
  const watch::ThemeSkinPart& p = theme().skin_part[part];
  const lv_color_t col = lv_color_hex(
      pressed && (p.set & (1u << 4)) ? p.text_pressed : p.text);
  skin_recolor(o, col);
}

// pressed 追従: LVGL は押下 state を子に伝播しないので、
// PRESSED/RELEASED/PRESS_LOST でラベルの色を直接切り替える。
void skin_press_cb(lv_event_t* e) {
  lv_obj_t* o = lv_event_get_target_obj(e);
  const int part = static_cast<int>(
      reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)) & 0xFF);
  skin_recolor_part(o, part, lv_obj_has_state(o, LV_STATE_PRESSED));
}

// o に part のスキンを貼る。画像があれば true (ベクタ背景はここで消す)。
// mode は skin_draw_cb の mode。use_pad=true で manifest pad を既存
// pad に加算 (画像の飾りがコンテンツに被らない内側余白)。
// text / text_pressed 指定時は中のラベルの文字色も差し替える。
bool skin_apply_obj(lv_obj_t* o, watch::ThemeSkinPartId part, int mode,
                    bool use_pad) {
  const Theme& t = theme();
  if (!(t.skin_set & (1u << part)) || !t.skin[part]) return false;
  const theme_res::SkinSet* ss = t.skin[part];
  if (!ss->img[0]) return false;
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_outline_width(o, 0, 0);
  lv_obj_set_style_shadow_width(o, 0, 0);
  const watch::ThemeSkinPart& p = t.skin_part[part];
  if (use_pad && (p.set & (1u << 2))) {
    lv_obj_set_style_pad_left(
        o, lv_obj_get_style_pad_left(o, LV_PART_MAIN) + p.pad[0], 0);
    lv_obj_set_style_pad_top(
        o, lv_obj_get_style_pad_top(o, LV_PART_MAIN) + p.pad[1], 0);
    lv_obj_set_style_pad_right(
        o, lv_obj_get_style_pad_right(o, LV_PART_MAIN) + p.pad[2], 0);
    lv_obj_set_style_pad_bottom(
        o, lv_obj_get_style_pad_bottom(o, LV_PART_MAIN) + p.pad[3], 0);
  }
  lv_obj_add_event_cb(o, skin_draw_cb, LV_EVENT_DRAW_MAIN,
                      skin_ud(part, mode));
  if (p.set & (3u << 3)) skin_recolor_part(o, part, false);
  if (p.set & (1u << 4)) {
    lv_obj_add_event_cb(o, skin_press_cb, LV_EVENT_PRESSED,
                        skin_ud(part, 0));
    lv_obj_add_event_cb(o, skin_press_cb, LV_EVENT_RELEASED,
                        skin_ud(part, 0));
    lv_obj_add_event_cb(o, skin_press_cb, LV_EVENT_PRESS_LOST,
                        skin_ud(part, 0));
  }
  return true;
}

// lv_switch のノブ/トラックをスキン画像で描く。ジオメトリは
// lv_switch.c の計算と同じ (アニメ中の位置は内部状態なので、
// 画像は CHECKED トグルで即座に切り替わる)。
void skin_switch_draw_cb(lv_event_t* e) {
  lv_obj_t* o = lv_event_get_target_obj(e);
  const Theme& t = theme();
  lv_layer_t* layer = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  const bool chk = (lv_obj_get_state(o) & LV_STATE_CHECKED) != 0;
  if (const theme_res::SkinSet* tr = t.skin[watch::kSkinSwitchTrack]) {
    const theme_res::SkinImg* im =
        chk && tr->img[2] ? tr->img[2] : tr->img[0];
    if (im) skin_draw_9(layer, im, &a);
  }
  if (const theme_res::SkinSet* kn = t.skin[watch::kSkinSwitchKnob]) {
    const theme_res::SkinImg* im =
        chk && kn->img[2] ? kn->img[2] : kn->img[0];
    if (im) {
      const int32_t ks = lv_area_get_height(&a);
      const int32_t len = lv_area_get_width(&a) - ks;
      lv_area_t k = a;
      k.x1 += chk ? len : 0;
      k.x2 = k.x1 + ks - 1;
      k.x1 -= lv_obj_get_style_pad_left(o, LV_PART_KNOB);
      k.x2 += lv_obj_get_style_pad_right(o, LV_PART_KNOB);
      k.y1 -= lv_obj_get_style_pad_top(o, LV_PART_KNOB);
      k.y2 += lv_obj_get_style_pad_bottom(o, LV_PART_KNOB);
      skin_draw_9(layer, im, &k);
    }
  }
}

// lv_slider の track/fill/knob をスキン画像で描く。ジオメトリは
// lv_bar.c draw_indic + lv_slider.c position_knob と同じ式
// (値は現在値のみ — ドラッグ中のアニメ途中値は参照しない)。
void skin_slider_draw_cb(lv_event_t* e) {
  lv_obj_t* o = lv_event_get_target_obj(e);
  const Theme& t = theme();
  lv_layer_t* layer = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(o, &a);

  if (const theme_res::SkinSet* tr = t.skin[watch::kSkinSliderTrack]) {
    if (tr->img[0]) skin_draw_9(layer, tr->img[0], &a);
  }

  // indic_area = coords − MAIN pad。
  lv_area_t ind = a;
  ind.x1 += lv_obj_get_style_pad_left(o, LV_PART_MAIN);
  ind.x2 -= lv_obj_get_style_pad_right(o, LV_PART_MAIN);
  ind.y1 += lv_obj_get_style_pad_top(o, LV_PART_MAIN);
  ind.y2 -= lv_obj_get_style_pad_bottom(o, LV_PART_MAIN);
  const int32_t mn = lv_slider_get_min_value(o);
  const int32_t mx = lv_slider_get_max_value(o);
  const int32_t rng = mx - mn;
  const int32_t pos =
      rng > 0 ? lv_area_get_width(&ind) * (lv_slider_get_value(o) - mn) / rng
              : 0;
  lv_area_t fill = ind;
  fill.x2 = ind.x1 + pos;  // lv_bar.c: *axis2 = *axis1 + anim_cur_value_x
  const bool pressed = (lv_obj_get_state(o) & LV_STATE_PRESSED) != 0;
  if (const theme_res::SkinSet* fl = t.skin[watch::kSkinSliderFill]) {
    const theme_res::SkinImg* im =
        pressed && fl->img[1] ? fl->img[1] : fl->img[0];
    if (im && pos > 0) skin_draw_9(layer, im, &fill);
  }
  if (const theme_res::SkinSet* kn = t.skin[watch::kSkinSliderKnob]) {
    const theme_res::SkinImg* im =
        pressed && kn->img[1] ? kn->img[1] : kn->img[0];
    if (im) {
      // position_knob: knob_size=obj高、中心=indic_area.x2、縦はcoords。
      const int32_t ks = lv_area_get_height(&a);
      lv_area_t k;
      k.x1 = fill.x2 - (ks >> 1);
      k.x2 = k.x1 + ks - 1;
      k.y1 = a.y1;
      k.y2 = a.y2;
      k.x1 -= lv_obj_get_style_pad_left(o, LV_PART_KNOB);
      k.x2 += lv_obj_get_style_pad_right(o, LV_PART_KNOB);
      k.y1 -= lv_obj_get_style_pad_top(o, LV_PART_KNOB);
      k.y2 += lv_obj_get_style_pad_bottom(o, LV_PART_KNOB);
      skin_draw_9(layer, im, &k);
    }
  }
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
  // ヘッダ帯の画像スキン (背景全面に描画。pad はレイアウトに効くので
  // ここでは加算しない)。
  skin_apply_obj(h, watch::kSkinHeaderBar, 0, false);

  const bool flat_hdr =
      (t.style.set & (1u << watch::kStyleHeader)) && t.style.header == 1;
  if (back_btn) {
    lv_obj_t* b = lv_button_create(h);
    clickable(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, kBackH);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    // style.header="flat" はピルの背景を消す (画像背景にかぶせる系)。
    lv_obj_set_style_bg_color(b, t.surface2, 0);
    if (flat_hdr) lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
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
    skin_apply_obj(b, watch::kSkinBackPill, 0, true);
  }

  lv_obj_t* tl = mk_label(h, title, t.font_title, t.text);
  (void)tl;
  lv_obj_set_user_data(h, kHeaderTag);
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
                    lv_opa_t bg_opa, lv_color_t fg,
                    watch::ThemeSkinPartId part) {
  const Theme& t = theme();
  lv_obj_t* b = lv_button_create(parent);
  clickable(b);
  lv_obj_set_size(b, LV_PCT(100), kBtnH);
  lv_obj_set_style_radius(b, skin_btn_radius(), 0);
  lv_obj_set_style_bg_color(b, bg, 0);
  lv_obj_set_style_bg_opa(b, bg_opa, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_t* l = mk_label(b, text, t.font_body, fg);
  lv_obj_center(l);
  skin_apply_obj(b, part, 0, true);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  return b;
}

}  // namespace

lv_obj_t* button(lv_obj_t* parent, const char* text, lv_event_cb_t cb,
                 void* ud) {
  const Theme& t = theme();
  return mk_button(parent, text, cb, ud, t.surface2, LV_OPA_COVER, t.text,
                   watch::kSkinBtnSecondary);
}

lv_obj_t* button_primary(lv_obj_t* parent, const char* text, lv_event_cb_t cb,
                        void* ud) {
  const Theme& t = theme();
  return mk_button(parent, text, cb, ud, t.primary, LV_OPA_COVER,
                   t.on_primary, watch::kSkinBtnPrimary);
}

lv_obj_t* button_danger(lv_obj_t* parent, const char* text, lv_event_cb_t cb,
                       void* ud) {
  const Theme& t = theme();
  // モックの rgba(danger, .16)。
  return mk_button(parent, text, cb, ud, t.danger, 41, t.danger,
                   watch::kSkinBtnDanger);
}

lv_obj_t* group(lv_obj_t* parent) {
  const Theme& t = theme();
  lv_obj_t* g = lv_obj_create(parent);
  bubble(g);
  lv_obj_set_size(g, LV_PCT(100), LV_SIZE_CONTENT);
  skin_card(g);
  lv_obj_set_style_pad_all(g, 0, 0);
  lv_obj_set_style_pad_row(g, 0, 0);  // pad_all では行間はゼロにならない
  lv_obj_set_flex_flow(g, LV_FLEX_FLOW_COLUMN);
  lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_user_data(g, kGroupTag);
  skin_apply_obj(g, watch::kSkinListGroup, 0, true);
  return g;
}

lv_obj_t* card(lv_obj_t* parent) {
  const Theme& t = theme();
  lv_obj_t* c = lv_obj_create(parent);
  bubble(c);
  lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
  skin_card(c);
  lv_obj_set_style_pad_all(c, 12, 0);
  lv_obj_set_style_pad_row(c, t.space, 0);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_START);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  skin_apply_obj(c, watch::kSkinCard, 0, true);
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
  skin_apply_obj(r, watch::kSkinRow, 0, true);
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
  return row_icon_img(grp, nullptr, icon, icon_bg, text, sub, chev, cb,
                      ud);
}

lv_obj_t* row_icon_img(lv_obj_t* grp, const lv_image_dsc_t* imgd,
                       const char* icon, lv_color_t icon_bg,
                       const char* text, const char* sub, bool chev,
                       lv_event_cb_t cb, void* ud) {
  const Theme& t = theme();
  lv_obj_t* r = row_box(grp, cb, ud);
  if (imgd) {
    // テーマのアイコン画像 (タイルを丸ごと置き換え)
    lv_obj_t* im = lv_image_create(r);
    lv_obj_set_size(im, kIconTile, kIconTile);
    lv_obj_set_style_radius(im, 11, 0);
    lv_obj_set_style_clip_corner(im, true, 0);
    lv_image_set_src(im, imgd);
    lv_obj_add_flag(im, LV_OBJ_FLAG_EVENT_BUBBLE);
  } else if (icon) {
    lv_obj_t* ic = flat(r);
    lv_obj_set_size(ic, kIconTile, kIconTile);
    lv_obj_set_style_radius(ic, 11, 0);
    lv_obj_set_style_bg_opa(ic, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(ic, icon_bg, 0);
    lv_obj_t* il = mk_label(ic, icon, t.font_body, lv_color_hex(0x000000));
    lv_obj_center(il);
    skin_apply_obj(ic, watch::kSkinIconTile, 0, false);
  }
  const bool has_icon = imgd || icon;
  row_text(r, text, sub, text_budget(has_icon, chev, false));
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
  // v3 skin: トラック/ノブ画像があればベクタ部品を透明化して画像を描く。
  const bool sk_tr = theme_skin(watch::kSkinSwitchTrack) != nullptr;
  const bool sk_kn = theme_skin(watch::kSkinSwitchKnob) != nullptr;
  if (sk_tr) {
    lv_obj_set_style_bg_opa(sw, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sw, LV_OPA_TRANSP,
                            LV_PART_INDICATOR | LV_STATE_CHECKED);
  }
  if (sk_kn) {
    lv_obj_set_style_bg_opa(sw, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, LV_OPA_TRANSP,
                            LV_PART_KNOB | LV_STATE_CHECKED);
  }
  if (sk_tr || sk_kn) {
    // ノブ画像のはみ出し分は KNOB pad が ext_draw_size に計上済み。
    lv_obj_add_event_cb(sw, skin_switch_draw_cb, LV_EVENT_DRAW_MAIN,
                        nullptr);
  }
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
  // v3 skin: caption_line はラベル下端の帯として描く。
  if (theme_skin(watch::kSkinCaptionLine)) {
    lv_obj_set_style_pad_bottom(l, 6, 0);
    skin_apply_obj(l, watch::kSkinCaptionLine, 1, false);
  }
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
// テーマ切替で色が変わるので、色が違ったら組み直す。
lv_grad_dsc_t s_sl_grad;
lv_color32_t s_sl_grad_c0 = {};
lv_color32_t s_sl_grad_c1 = {};
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
  // MAIN pad は LVGL がトラック (indic_area) と値マッピングの両方を
  // 内側にずらすのに使う (lv_bar.c)。ノブ半径分を左右に入れて、
  // 最小/最大でノブがトラック端より外に切れるのを防ぐ。
  lv_obj_set_style_pad_left(s, kKnob / 2, LV_PART_MAIN);
  lv_obj_set_style_pad_right(s, kKnob / 2, LV_PART_MAIN);
  lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s, t.line, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
  // 塗りは primary→primary2 の横グラデーション (色が変わったら組み直し)。
  if (!s_sl_grad_init ||
      s_sl_grad_c0.red != t.primary.red ||
      s_sl_grad_c0.green != t.primary.green ||
      s_sl_grad_c0.blue != t.primary.blue ||
      s_sl_grad_c1.red != t.primary2.red ||
      s_sl_grad_c1.green != t.primary2.green ||
      s_sl_grad_c1.blue != t.primary2.blue) {
    const lv_color_t cols[2] = {t.primary, t.primary2};
    const lv_opa_t opas[2] = {LV_OPA_COVER, LV_OPA_COVER};
    const uint8_t fr[2] = {0, 255};
    lv_grad_init_stops(&s_sl_grad, cols, opas, fr, 2);
    lv_grad_linear_init(&s_sl_grad, LV_GRAD_LEFT, LV_GRAD_CENTER,
                        LV_GRAD_RIGHT, LV_GRAD_CENTER, LV_GRAD_EXTEND_PAD);
    s_sl_grad_c0 = lv_color_to_32(t.primary, LV_OPA_COVER);
    s_sl_grad_c1 = lv_color_to_32(t.primary2, LV_OPA_COVER);
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
  // v3 skin: トラック/塗り/ノブ画像があればベクタ部品を透明化して画像を描く。
  const bool sk_tr = theme_skin(watch::kSkinSliderTrack) != nullptr;
  const bool sk_fl = theme_skin(watch::kSkinSliderFill) != nullptr;
  const bool sk_kn = theme_skin(watch::kSkinSliderKnob) != nullptr;
  if (sk_tr) lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, LV_PART_MAIN);
  if (sk_fl) lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, LV_PART_INDICATOR);
  if (sk_kn) {
    lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_outline_opa(s, LV_OPA_TRANSP, LV_PART_KNOB);
  }
  if (sk_tr || sk_fl || sk_kn) {
    // ノブ画像のはみ出し分は KNOB pad が ext_draw_size に計上済み。
    lv_obj_add_event_cb(s, skin_slider_draw_cb, LV_EVENT_DRAW_MAIN,
                        nullptr);
  }
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

// ---- 画面隅マスコット (manifest "mascot") ----
namespace {

// 1画面1個想定だが Home が常駐するので最大2つ。静的プール (削除時に解放)。
struct MascotSt {
  lv_obj_t* img;    // 表情画像
  lv_obj_t* bub;    // セリフ吹き出し
  lv_obj_t* lbl;    // 吹き出し内テキスト
  lv_timer_t* hide; // 自動で隠すタイマー
  uint8_t expr;     // 現在の表情 index
  bool small;       // ヘッダ画面の小型表示か
  bool used;
};
MascotSt s_ms[2] = {};

// ヘッダ画面での小型マスコットの高さ (タイトルと同じ高さに収める)。
constexpr int32_t kMascotSmall = 44;

void mascot_hide(MascotSt* st) {
  if (st->bub) lv_obj_add_flag(st->bub, LV_OBJ_FLAG_HIDDEN);
  if (st->hide) {
    lv_timer_delete(st->hide);
    st->hide = nullptr;
  }
}

void mascot_hide_cb(lv_timer_t* tm) {
  mascot_hide(static_cast<MascotSt*>(lv_timer_get_user_data(tm)));
}

void mascot_tap(lv_event_t* e) {
  MascotSt* st =
      static_cast<MascotSt*>(lv_event_get_user_data(e));
  const Theme& t = theme();
  // 表情を順送り (expr_count=1 なら固定)。
  if (t.mascot.expr_count > 1) {
    st->expr = static_cast<uint8_t>((st->expr + 1) % t.mascot.expr_count);
    if (t.mascot_dsc[st->expr]) {
      lv_image_set_src(st->img, t.mascot_dsc[st->expr]);
    }
  }
  // セリフはランダム (同じの連続は避ける)。
  if (t.mascot.line_count > 0) {
    const uint32_t i = lv_rand(0, t.mascot.line_count - 1);
    lv_label_set_text(st->lbl, t.mascot.line[i]);
    lv_obj_remove_flag(st->bub, LV_OBJ_FLAG_HIDDEN);
    if (st->small) {
      // 小型表示: ヘッダ直下の右上にトースト。コンテンツ上端を塞がない。
      lv_obj_align(st->bub, LV_ALIGN_TOP_RIGHT, -22, kHeaderH + 6);
    } else {
      // 隅の大型表示: 吹き出しは画像の直上 (OUT_TOP で吸着)。
      lv_obj_align_to(st->bub, st->img, LV_ALIGN_OUT_TOP_MID, 0, -8);
    }
    if (st->hide) lv_timer_delete(st->hide);
    st->hide = lv_timer_create(mascot_hide_cb, 3000, st);
    lv_timer_set_repeat_count(st->hide, 1);
  }
}

void mascot_free(lv_event_t* e) {
  MascotSt* st =
      static_cast<MascotSt*>(lv_event_get_user_data(e));
  if (st->hide) lv_timer_delete(st->hide);
  st->used = false;
}

}  // namespace

lv_obj_t* mascot(lv_obj_t* scr, int screen_index) {
  const Theme& t = theme();
  if (!ui::theme_mascot_on(screen_index) || !t.mascot_dsc[0]) {
    return nullptr;
  }
  MascotSt* st = nullptr;
  for (auto& s : s_ms) {
    if (!s.used) {
      st = &s;
      break;
    }
  }
  if (!st) return nullptr;
  st->used = true;
  st->hide = nullptr;
  st->expr = 0;

  // ヘッダがある画面 (= リスト系) はマスコットをヘッダ右側に小型表示し、
  // コンテンツとは一切重ならないようにする。ヘッダの無い画面
  // (face/alert などレイアウトが場所を確保するもの) だけ原寸で隅に置く。
  bool has_header = false;
  const uint32_t nch = lv_obj_get_child_count(scr);
  for (uint32_t i = 0; i < nch; ++i) {
    if (lv_obj_get_user_data(lv_obj_get_child(scr,
                                            static_cast<int32_t>(i))) ==
        kHeaderTag) {
      has_header = true;
      break;
    }
  }
  st->small = has_header;

  const int32_t w = t.mascot_dsc[0]->header.w;
  const int32_t h = t.mascot_dsc[0]->header.h;
  lv_obj_t* im = lv_image_create(scr);
  lv_image_set_src(im, t.mascot_dsc[0]);
  int32_t x, y;
  if (has_header) {
    // 右端から 22px 以上・タイトルと同じ高さ (ピル中心 ~y41)。
    lv_obj_set_size(im, kMascotSmall, kMascotSmall);
    lv_image_set_scale(im, static_cast<int32_t>(
                               kMascotSmall * LV_SCALE_NONE / w));
    x = kW - 22 - kMascotSmall;
    y = 41 - kMascotSmall / 2;
  } else {
    // x/y<0 は右/下端基準のオフセット。
    x = t.mascot.x < 0 ? kW + t.mascot.x - w : t.mascot.x;
    y = t.mascot.y < 0 ? 502 + t.mascot.y - h : t.mascot.y;
    // 丸角 (~44px) の内側の安全域に収める: 端から 22px 以内は使わない。
    if (x < 22) x = 22;
    if (x > kW - 22 - w) x = kW - 22 - w;
    if (y < 22) y = 22;
    if (y > 502 - 22 - h) y = 502 - 22 - h;
  }
  lv_obj_set_pos(im, x, y);
  clickable(im);
  lv_obj_add_event_cb(im, mascot_tap, LV_EVENT_CLICKED, st);
  // 画面が消えるときプールを解放 (タイマーが残って DANGLING しないよう)。
  lv_obj_add_event_cb(im, mascot_free, LV_EVENT_DELETE, st);
  st->img = im;

  // 吹き出し (最初は非表示)。上に張り付くので画面座標で置く。
  lv_obj_t* bub = flat(scr);
  lv_obj_set_size(bub, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_max_width(bub, 190, 0);
  lv_obj_set_style_radius(bub, 14, 0);
  lv_obj_set_style_bg_color(bub, t.surface2, 0);
  lv_obj_set_style_bg_opa(bub, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_left(bub, 10, 0);
  lv_obj_set_style_pad_right(bub, 10, 0);
  lv_obj_set_style_pad_top(bub, 6, 0);
  lv_obj_set_style_pad_bottom(bub, 6, 0);
  lv_obj_t* lbl = mk_label(bub, "", t.font_body, t.text);
  lv_obj_set_width(lbl, 170);
  // 吹き出し自身はタップを取らない (出ている間に下の操作を塞がない)。
  lv_obj_remove_flag(bub, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(bub, LV_OBJ_FLAG_HIDDEN);
  skin_apply_obj(bub, watch::kSkinToast, 0, true);
  st->bub = bub;
  st->lbl = lbl;
  return im;
}

bool skin_obj(lv_obj_t* o, watch::ThemeSkinPartId part) {
  return skin_apply_obj(o, part, 0, true);
}

}  // namespace ui::c
