// face_theme.cpp — 「theme」文字盤 (manifest face_layout 駆動)。
//   テーマ作者が face_layout で要素の位置・フォント・色を自由に配置する。
//   何も指定しなければ最小構成で中央に時刻だけ出す (docs/theme-format.md)。
//
//   x,y: >=0 は左上基準の座標。<0 は右/下端基準のオフセット
//        (例: x=-20 → 右端から 20px 内側)。
//   font: px ヒント → {<=23:body, <=41:title, <=76:digits_sm, else:digits}。
//        テーマの fonts キーで読み込んだ binfont があればそれが効く。
//   color: 0xRRGGBB (省略時 text)。
//   shadow / shadow_dx,dy: 同文字を背面にずらして敷く (v6)。
//   time_h / time_m: 時・分を別要素で置く (v6)。
//   battery_gauge: 電池残量を色付きバーで描く (v6)。
#include "faces.hpp"

#include <cstdio>

#include "../components.hpp"
#include "../theme.hpp"
#include "ui/face_data.hpp"
#include "ui/port.hpp"
#include "watch/safe_area.hpp"

namespace {

// 画面内の静的参照 (一度に1文字盤しか生きないので静的でよい)。
struct St {
  lv_obj_t* lbl[watch::kThemeFaceElemCount] = {};
  lv_obj_t* shd[watch::kThemeFaceElemCount] = {};
  lv_obj_t* ring[watch::kThemeFaceElemCount][4] = {};
  lv_obj_t* hl[watch::kThemeFaceElemCount] = {};
  lv_obj_t* gauge = nullptr;
  lv_obj_t* gauge_fill = nullptr;
  lv_obj_t* gauge_cap = nullptr;
};
St s;

// font px ヒント → Theme のフォントスロット。
const lv_font_t* pick_font(const ui::Theme& t, int16_t px) {
  if (px <= 23) return t.font_body;
  if (px <= 41) return t.font_title;
  if (px <= 76) return t.font_digits_sm;
  return t.font_digits;
}

lv_color_t pick_color(const ui::Theme& t, const watch::ThemeFaceElem& e) {
  return (e.set & (1u << 5)) ? lv_color_hex(e.color) : t.text;
}

// 座標/端基準の align 計算。clamp=true なら、マニフェストの座標が
// 丸角の安全域外になるとき内側へ寄せる (装飾の chara 立ち絵は対象外)。
void place(lv_obj_t* o, const watch::ThemeFaceElem& e, bool clamp = true) {
  lv_align_t a;
  if (e.x < 0 && e.y < 0) a = LV_ALIGN_BOTTOM_RIGHT;
  else if (e.x < 0) a = LV_ALIGN_TOP_RIGHT;
  else if (e.y < 0) a = LV_ALIGN_BOTTOM_LEFT;
  else a = LV_ALIGN_TOP_LEFT;
  lv_obj_align(o, a, e.x, e.y);
  if (!clamp) return;
  // サイズ確定後に角が安全域に入るまで中央方向へ寄せる。
  lv_obj_update_layout(o);
  lv_area_t b;
  lv_obj_get_coords(o, &b);
  const int w = lv_area_get_width(&b), h = lv_area_get_height(&b);
  int x = b.x1, y = b.y1;
  for (int i = 0;
       i < 96 && !watch::rect_in_safe_area(x, y, w - 1, h - 1); ++i) {
    if (!watch::point_in_safe_area(x, y)) { ++x; ++y; }
    if (!watch::point_in_safe_area(x + w - 1, y)) { --x; ++y; }
    if (!watch::point_in_safe_area(x, y + h - 1)) { ++x; --y; }
    if (!watch::point_in_safe_area(x + w - 1, y + h - 1)) { --x; --y; }
  }
  if (x != b.x1 || y != b.y1) lv_obj_set_pos(o, x, y);
}

int elem_dx(const watch::ThemeFaceElem& e) {
  return (e.set & (1u << 14)) ? e.shadow_dx : 4;
}
int elem_dy(const watch::ThemeFaceElem& e) {
  return (e.set & (1u << 15)) ? e.shadow_dy : 5;
}

// 影を背面に持つテキスト要素を作る。影は本体の親にして描画順を
// 「影 → 本体」に固定し、sim の重なり監査でも親子として除外される。
lv_obj_t* elem_text(lv_obj_t* scr, const ui::Theme& t, int idx) {
  const watch::ThemeFaceElem& e = t.face_elem[idx];
  const lv_font_t* f =
      (e.set & (1u << 4)) ? pick_font(t, e.font) : t.font_body;
  const bool has_shadow = (e.set & (1u << 9)) != 0;
  const bool has_ring = (e.set & (1u << 16)) != 0;
  const bool has_hl = (e.set & (1u << 19)) != 0;
  if (!has_shadow && !has_ring && !has_hl) {
    lv_obj_t* l = ui::face::label(scr, "", f, pick_color(t, e));
    place(l, e);
    s.lbl[idx] = l;
    return l;
  }
  // 影と縁取りは本体の親チェーンとして背面に積む (親子なので重なり監査の
  // 対象外、描画順も 影→縁取り→本体 に固定される)。
  const int dx = has_shadow ? elem_dx(e) : 0;
  const int dy = has_shadow ? elem_dy(e) : 0;
  const int w = (e.set & (1u << 17)) ? e.outline_w : 2;
  lv_obj_t* parent = nullptr;
  int px = 0, py = 0;
  auto layer = [&](int ox, int oy, lv_color_t col) {
    lv_obj_t* l;
    if (!parent) {
      l = ui::face::label(scr, "", f, col);
      lv_obj_add_flag(l, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
      place(l, e);
      lv_obj_set_pos(l, lv_obj_get_x(l) + ox, lv_obj_get_y(l) + oy);
    } else {
      l = ui::face::label(parent, "", f, col);
      lv_obj_add_flag(parent, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
      lv_obj_set_pos(l, ox - px, oy - py);
    }
    parent = l;
    px = ox;
    py = oy;
    return l;
  };
  if (has_shadow) s.shd[idx] = layer(dx, dy, lv_color_hex(e.shadow));
  if (has_hl) {
    // 上光: 本体より |dy| 上に同色系の明るい文字を置し、本体が
    // それを覆う。見えるのは各ストロークの上端だけ (光が上から当たる)。
    const int hdy = (e.set & (1u << 20)) ? e.highlight_dy : -4;
    s.hl[idx] = layer(0, hdy, lv_color_hex(e.highlight));
  }
  if (has_ring) {
    const lv_color_t rc = lv_color_hex(e.outline);
    s.ring[idx][0] = layer(0, -w, rc);
    s.ring[idx][1] = layer(0, w, rc);
    s.ring[idx][2] = layer(w, 0, rc);
    s.ring[idx][3] = layer(-w, 0, rc);
  }
  lv_obj_t* l = ui::face::label(parent, "", f, pick_color(t, e));
  lv_obj_set_pos(l, -px, -py);
  s.lbl[idx] = l;
  return l;
}

void set_text(int idx, const char* text) {
  if (!s.lbl[idx]) return;
  lv_label_set_text(s.lbl[idx], text);
  if (s.shd[idx]) lv_label_set_text(s.shd[idx], text);
  for (int k = 0; k < 4; ++k) {
    if (s.ring[idx][k]) lv_label_set_text(s.ring[idx][k], text);
  }
  if (s.hl[idx]) lv_label_set_text(s.hl[idx], text);
}

// "19:07" を時・分に割る (fmt_hm は常に HH:MM)。
void split_hm(char* h, char* m) {
  char b[8];
  ui::face::fmt_hm(b, sizeof(b), ui::face::now());
  h[0] = b[0];
  h[1] = b[1];
  h[2] = '\0';
  m[0] = b[3];
  m[1] = b[4];
  m[2] = '\0';
}

void set_time(int idx) {
  char b[8];
  ui::face::fmt_hm(b, sizeof(b), ui::face::now());
  set_text(idx, b);
}

void set_hour(int idx) {
  char h[3], m[3];
  split_hm(h, m);
  set_text(idx, h);
}

void set_minute(int idx) {
  char h[3], m[3];
  split_hm(h, m);
  set_text(idx, m);
}

void set_date(int idx) {
  char b[32];
  ui::face::fmt_date_jp(b, sizeof(b), ui::face::now());
  set_text(idx, b);
}

void set_steps(int idx) {
  const ui::face_data::Snapshot d = ui::face_data::get();
  char nb[16], b[24];
  ui::face::fmt_steps(nb, sizeof(nb), d.steps);
  std::snprintf(b, sizeof(b), "%s 歩", nb);
  set_text(idx, b);
}

void set_batt(int idx) {
  char nb[8], b[12];
  ui::face::fmt_battery(nb, sizeof(nb), ui::port::battery_percent());
  std::snprintf(b, sizeof(b), "%s%%", nb);
  set_text(idx, b);
}

void set_notify(int idx) {
  const ui::face_data::Snapshot d = ui::face_data::get();
  char b[8];
  int n = static_cast<int>(d.notifications);
  if (n < 0) n = 0;
  if (n > 99) n = 99;
  std::snprintf(b, sizeof(b), "%d", n);
  set_text(idx, b);
}

// ---- v6: 電池ゲージ ----

lv_color_t gauge_fill_color(const ui::Theme& t,
                            const watch::ThemeFaceElem& e, bool charging) {
  if (charging && (e.set & (1u << 8))) return lv_color_hex(e.charging);
  if (e.set & (1u << 5)) return lv_color_hex(e.color);
  return t.primary;
}

void gauge_update(const ui::Theme& t, const watch::ThemeFaceElem& e) {
  if (!s.gauge || !s.gauge_fill) return;
  int pct = ui::port::battery_percent();
  const bool charging = ui::port::battery_charging();
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  const int w = lv_obj_get_width(s.gauge);
  const int h = lv_obj_get_height(s.gauge);
  const bool vert = (e.set & (1u << 11)) && e.vertical;
  int fw = vert ? w : (w * pct + 50) / 100;
  int fh = vert ? (h * pct + 50) / 100 : h;
  int fx = 0, fy = 0;
  if (vert) {
    fy = h - fh;
  }
  if (pct == 0) {
    fw = 0;
    fh = 0;
  }
  lv_obj_set_pos(s.gauge_fill, fx, fy);
  lv_obj_set_size(s.gauge_fill, fw, fh);
  const int r = (e.set & (1u << 10)) ? e.radius : 5;
  const int fr = LV_MIN(r, LV_MIN(fw, fh) / 2);
  lv_obj_set_style_radius(s.gauge_fill, fr > 0 ? fr - (fr > 1 ? 1 : 0) : 0, 0);
  lv_obj_set_style_bg_color(s.gauge_fill, gauge_fill_color(t, e, charging), 0);
}

void build_gauge(lv_obj_t* scr, const ui::Theme& t,
                 const watch::ThemeFaceElem& e) {
  const int w = e.w > 0 ? e.w : 48;
  const int h = e.h > 0 ? e.h : 20;
  const int r = (e.set & (1u << 10)) ? e.radius : 5;
  const bool has_track = (e.set & (1u << 7)) != 0;

  lv_obj_t* tr = lv_obj_create(scr);
  lv_obj_set_size(tr, w, h);
  lv_obj_set_style_radius(tr, r, 0);
  lv_obj_set_style_bg_opa(tr, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(
      tr, has_track ? lv_color_hex(e.track) : t.surface2, 0);
  lv_obj_set_style_border_width(tr, has_track ? 0 : 1, 0);
  lv_obj_set_style_border_color(tr, t.edge, 0);
  lv_obj_set_style_pad_all(tr, 0, 0);
  lv_obj_remove_flag(tr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(tr, LV_OBJ_FLAG_EVENT_BUBBLE);
  place(tr, e);
  s.gauge = tr;

  lv_obj_t* fl = lv_obj_create(tr);
  lv_obj_set_style_border_width(fl, 0, 0);
  lv_obj_set_style_bg_opa(fl, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(fl, 0, 0);
  lv_obj_remove_flag(fl, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(fl, LV_OBJ_FLAG_EVENT_BUBBLE);
  s.gauge_fill = fl;

  if (e.set & (1u << 12)) {
    lv_area_t a;
    lv_obj_get_coords(tr, &a);
    const int cw = LV_MAX(4, h / 4);
    const int ch = LV_MAX(6, (h * 2) / 5);
    lv_obj_t* cap = lv_obj_create(scr);
    lv_obj_set_size(cap, cw, ch);
    lv_obj_set_pos(cap, a.x2 + 2, a.y1 + (h - ch) / 2);
    lv_obj_set_style_radius(cap, 2, 0);
    lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(
        cap, has_track ? lv_color_hex(e.track) : t.edge, 0);
    lv_obj_set_style_border_width(cap, 0, 0);
    lv_obj_remove_flag(cap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cap, LV_OBJ_FLAG_EVENT_BUBBLE);
    s.gauge_cap = cap;
  }
  gauge_update(t, e);
}

void build_text_elem(lv_obj_t* scr, const ui::Theme& t, int i) {
  elem_text(scr, t, i);
  switch (i) {
    case watch::kThemeFaceElemTime: set_time(i); break;
    case watch::kThemeFaceElemTimeH: set_hour(i); break;
    case watch::kThemeFaceElemTimeM: set_minute(i); break;
    case watch::kThemeFaceElemDate: set_date(i); break;
    case watch::kThemeFaceElemSteps: set_steps(i); break;
    case watch::kThemeFaceElemBattery: set_batt(i); break;
    case watch::kThemeFaceElemNotify: set_notify(i); break;
    default: break;
  }
}

lv_obj_t* build(lv_obj_t* scr) {
  const ui::Theme& t = ui::theme();
  s = St{};

  // behind=1 の要素は立ち絵の背面に置く (時計の数字をキャラの後ろへ)。
  for (int i = 0; i < watch::kThemeFaceElemCount; ++i) {
    if (!(t.face_layout_set & (1u << i))) continue;
    if (!(t.face_elem[i].set & (1u << 18))) continue;
    if (i == watch::kThemeFaceElemBatteryGauge ||
        i == watch::kThemeFaceElemChara || i == watch::kThemeFaceElemBubble)
      continue;
    build_text_elem(scr, t, i);
  }

  if (t.face_layout_set & (1u << watch::kThemeFaceElemChara)) {
    const watch::ThemeFaceElem& e = t.face_elem[watch::kThemeFaceElemChara];
    const lv_image_dsc_t* img = t.face_chara_dsc ? t.face_chara_dsc
                                : t.img_face_chara ? t.img_face_chara
                                                   : t.img_stand;
    if (img) {
      lv_obj_t* im = lv_image_create(scr);
      lv_image_set_src(im, img);
      lv_obj_add_flag(im, LV_OBJ_FLAG_EVENT_BUBBLE);
      place(im, e, false);  // 立ち絵は装飾: 角で切れてよい
    }
  }

  // gauge を先に作る (重ねた battery テキストを後から上に描く)。
  for (int i = 0; i < watch::kThemeFaceElemCount; ++i) {
    if (!(t.face_layout_set & (1u << i))) continue;
    if (i != watch::kThemeFaceElemBatteryGauge) continue;
    if (t.face_elem[i].set & (1u << 18)) continue;
    build_gauge(scr, t, t.face_elem[i]);
  }

  for (int i = 0; i < watch::kThemeFaceElemCount; ++i) {
    if (!(t.face_layout_set & (1u << i))) continue;
    if (t.face_elem[i].set & (1u << 18)) continue;
    if (i == watch::kThemeFaceElemChara) continue;
    if (i == watch::kThemeFaceElemBatteryGauge) continue;
    build_text_elem(scr, t, i);
  }

  // bubble: ふきだし箱 + 時刻帯の文言 (theme.bubble[])。
  if (t.face_layout_set & (1u << watch::kThemeFaceElemBubble)) {
    const watch::ThemeFaceElem& e = t.face_elem[watch::kThemeFaceElemBubble];
    lv_obj_t* box = lv_obj_create(scr);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(box, 300, 0);
    lv_obj_set_style_radius(box, 16, 0);
    lv_obj_set_style_bg_color(box, t.bubble_bg, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_left(box, 12, 0);
    lv_obj_set_style_pad_right(box, 12, 0);
    lv_obj_set_style_pad_top(box, 8, 0);
    lv_obj_set_style_pad_bottom(box, 8, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_EVENT_BUBBLE);
    ui::c::skin_obj(box, watch::kSkinBubble);
    lv_obj_t* l = ui::face::label(box, t.bubble[0],
                                  (e.set & (1u << 4)) ? pick_font(t, e.font)
                                                      : t.font_body,
                                  (e.set & (1u << 5))
                                      ? lv_color_hex(e.color)
                                      : t.bubble_text);
    lv_obj_set_width(l, 276);
    place(box, e);
  }

  // face_layout が何も無いテーマ: ヒントだけ中央に出す。
  if (!t.face_layout_set) {
    lv_obj_t* l =
        ui::face::label(scr, "face_layout 未設定", t.font_body, t.text_dim);
    lv_obj_center(l);
  }
  return scr;
}

void on_event(const watch::Event& e) {
  const ui::Theme& t = ui::theme();
  switch (e.type) {
    case watch::EventType::ClockTick:
      set_time(watch::kThemeFaceElemTime);
      set_hour(watch::kThemeFaceElemTimeH);
      set_minute(watch::kThemeFaceElemTimeM);
      set_date(watch::kThemeFaceElemDate);
      break;
    case watch::EventType::StepsChanged:
      set_steps(watch::kThemeFaceElemSteps);
      break;
    case watch::EventType::BatteryChanged:
      set_batt(watch::kThemeFaceElemBattery);
      gauge_update(t, t.face_elem[watch::kThemeFaceElemBatteryGauge]);
      break;
    case watch::EventType::NotificationPosted:
    case watch::EventType::NotificationsCleared:
      set_notify(watch::kThemeFaceElemNotify);
      break;
    default:
      break;
  }
}

}  // namespace

extern const ui::face::Ops kFaceOpsTheme = {"theme", build, on_event};
