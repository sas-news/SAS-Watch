// face_theme.cpp — 「theme」文字盤 (manifest face_layout 駆動)。
//   テーマ作者が face_layout で time/date/steps/battery/notify/bubble/chara
//   の位置・フォント・色を自由に配置する。何も指定しなければ最小構成で
//   中央に時刻だけ出す (docs/theme-format.md v2)。
//
//   x,y: >=0 は左上基準の座標。<0 は右/下端基準のオフセット
//        (例: x=-20 → 右端から 20px 内側)。
//   font: px ヒント → {<=23:body, <=41:title, <=76:digits_sm, else:digits}。
//        テーマの fonts キーで読み込んだ binfont があればそれが効く。
//   color: 0xRRGGBB (省略時 text / time は text)。
#include "faces.hpp"

#include <cstdio>

#include "../theme.hpp"
#include "ui/face_data.hpp"
#include "ui/port.hpp"

namespace {

// 画面内の静的参照 (一度に1文字盤しか生きないので静的でよい)。
struct St {
  lv_obj_t* time_l = nullptr;
  lv_obj_t* date_l = nullptr;
  lv_obj_t* steps_l = nullptr;
  lv_obj_t* batt_l = nullptr;
  lv_obj_t* notify_l = nullptr;
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

// 座標/端基準の align 計算。
void place(lv_obj_t* o, const watch::ThemeFaceElem& e) {
  lv_align_t a;
  if (e.x < 0 && e.y < 0) a = LV_ALIGN_BOTTOM_RIGHT;
  else if (e.x < 0) a = LV_ALIGN_TOP_RIGHT;
  else if (e.y < 0) a = LV_ALIGN_BOTTOM_LEFT;
  else a = LV_ALIGN_TOP_LEFT;
  lv_obj_align(o, a, e.x, e.y);
}

void set_time(lv_obj_t* l) {
  char b[8];
  ui::face::fmt_hm(b, sizeof(b), ui::face::now());
  lv_label_set_text(l, b);
}

void set_date(lv_obj_t* l) {
  char b[32];
  ui::face::fmt_date_jp(b, sizeof(b), ui::face::now());
  lv_label_set_text(l, b);
}

void set_steps(lv_obj_t* l) {
  const ui::face_data::Snapshot d = ui::face_data::get();
  char nb[16], b[24];
  ui::face::fmt_steps(nb, sizeof(nb), d.steps);
  std::snprintf(b, sizeof(b), "%s 歩", nb);
  lv_label_set_text(l, b);
}

void set_batt(lv_obj_t* l) {
  char nb[8], b[12];
  ui::face::fmt_battery(nb, sizeof(nb), ui::port::battery_percent());
  std::snprintf(b, sizeof(b), "%s%%", nb);
  lv_label_set_text(l, b);
}

void set_notify(lv_obj_t* l) {
  const ui::face_data::Snapshot d = ui::face_data::get();
  char b[8];
  std::snprintf(b, sizeof(b), "%d",
                d.notifications < 0 ? 0 : static_cast<int>(d.notifications));
  lv_label_set_text(l, b);
}

// 要素のラベルを1個作る (time/date/steps/battery/notify 用)。
lv_obj_t* elem_label(lv_obj_t* scr, const ui::Theme& t, int idx) {
  const watch::ThemeFaceElem& e = t.face_elem[idx];
  const char* init = "";
  lv_obj_t* l = ui::face::label(scr, init,
                              (e.set & (1u << 4)) ? pick_font(t, e.font)
                                                  : t.font_body,
                              pick_color(t, e));
  place(l, e);
  return l;
}

lv_obj_t* build(lv_obj_t* scr) {
  const ui::Theme& t = ui::theme();
  s = St{};

  if (t.face_layout_set & (1u << watch::kThemeFaceElemTime)) {
    s.time_l = elem_label(scr, t, watch::kThemeFaceElemTime);
    set_time(s.time_l);
  }
  if (t.face_layout_set & (1u << watch::kThemeFaceElemDate)) {
    s.date_l = elem_label(scr, t, watch::kThemeFaceElemDate);
    set_date(s.date_l);
  }
  if (t.face_layout_set & (1u << watch::kThemeFaceElemSteps)) {
    s.steps_l = elem_label(scr, t, watch::kThemeFaceElemSteps);
    set_steps(s.steps_l);
  }
  if (t.face_layout_set & (1u << watch::kThemeFaceElemBattery)) {
    s.batt_l = elem_label(scr, t, watch::kThemeFaceElemBattery);
    set_batt(s.batt_l);
  }
  if (t.face_layout_set & (1u << watch::kThemeFaceElemNotify)) {
    s.notify_l = elem_label(scr, t, watch::kThemeFaceElemNotify);
    set_notify(s.notify_l);
  }

  // bubble: ふきだし箱 + 時刻帯の文言 (theme.bubble[])。
  if (t.face_layout_set & (1u << watch::kThemeFaceElemBubble)) {
    const watch::ThemeFaceElem& e =
        t.face_elem[watch::kThemeFaceElemBubble];
    lv_obj_t* box = lv_obj_create(scr);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(box, 220, 0);
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
    lv_obj_t* l = ui::face::label(box, t.bubble[0],
                                  (e.set & (1u << 4)) ? pick_font(t, e.font)
                                                      : t.font_body,
                                  (e.set & (1u << 5))
                                      ? lv_color_hex(e.color)
                                      : t.bubble_text);
    lv_obj_set_width(l, 200);
    place(box, e);
  }

  // chara: face_layout.chara.img → face_chara_dsc、無ければ既存スロット。
  if (t.face_layout_set & (1u << watch::kThemeFaceElemChara)) {
    const watch::ThemeFaceElem& e =
        t.face_elem[watch::kThemeFaceElemChara];
    const lv_image_dsc_t* img = t.face_chara_dsc ? t.face_chara_dsc
                                : t.img_face_chara ? t.img_face_chara
                                                   : t.img_stand;
    if (img) {
      lv_obj_t* im = lv_image_create(scr);
      lv_image_set_src(im, img);
      lv_obj_add_flag(im, LV_OBJ_FLAG_EVENT_BUBBLE);
      place(im, e);
    }
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
  // 時刻・日付・計測値の定期更新。
  switch (e.type) {
    case watch::EventType::ClockTick:
      if (s.time_l) set_time(s.time_l);
      if (s.date_l) set_date(s.date_l);
      break;
    case watch::EventType::StepsChanged:
      if (s.steps_l) set_steps(s.steps_l);
      break;
    case watch::EventType::BatteryChanged:
      if (s.batt_l) set_batt(s.batt_l);
      break;
    case watch::EventType::NotificationPosted:
    case watch::EventType::NotificationsCleared:
      if (s.notify_l) set_notify(s.notify_l);
      break;
    default:
      break;
  }
}

}  // namespace

extern const ui::face::Ops kFaceOpsTheme = {"theme", build, on_event};
