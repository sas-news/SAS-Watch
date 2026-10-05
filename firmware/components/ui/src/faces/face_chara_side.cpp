// face_chara_side.cpp — 「chara_side」文字盤。
//   右に立ち絵 (テーマ画像 face_chara、無ければ stand そのままの大きさ)、
//   背後に放射状のグロー、左に時(太)/分(アクセント)の縦積みと短い線、
//   左下に電池・歩数。
//   画像が無いテーマでは「キャラなし」簡易レイアウト (時刻中央) に倒す。
#include "faces.hpp"

#include <cstdio>

#include "ui/face_data.hpp"
#include "ui/port.hpp"
#include "watch/power.hpp"

namespace {
using ui::face::Now;

struct S {
  bool has_img = false;
  lv_obj_t* date = nullptr;
  lv_obj_t* hour = nullptr;
  lv_obj_t* min = nullptr;
  lv_obj_t* batt = nullptr;    // 「電池」+ 値
  lv_obj_t* batt_v = nullptr;
  lv_obj_t* steps = nullptr;   // 「歩数」+ 値
  lv_obj_t* steps_v = nullptr;
  int32_t last_min = -1;
  int32_t last_batt = -1;
  int32_t last_steps = -2;
  bool screen_off = false;
};
S s;

// 背後のグロー: 大きい円から小さい円へ不透明度を上げて重ねると、
// 1枚ずつの円の輪郭が見えてしまう。半径を数px刻みで多数重ねて
// なめらかな放射状に見せる (静的装飾: 描画は1回だけ)。
void draw_glow(lv_event_t* e) {
  lv_layer_t* layer = lv_event_get_layer(e);
  const ui::Theme& t = ui::theme();
  constexpr int kMaxR = 170;
  constexpr int kStep = 4;
  lv_draw_rect_dsc_t r;
  lv_draw_rect_dsc_init(&r);
  r.bg_color = t.accent4;
  r.radius = LV_RADIUS_CIRCLE;
  r.border_width = 0;
  for (int rad = kMaxR; rad > kStep; rad -= kStep) {
    // 中心に近いほど濃く (二乗でなだらかに減衰)。
    const float f = 1.f - static_cast<float>(rad) / kMaxR;
    r.bg_opa = static_cast<lv_opa_t>(LV_OPA_10 +
                                     f * f * (LV_OPA_70 - LV_OPA_10));
    lv_area_t a = {295 - rad, 226 - rad, 295 + rad - 1, 226 + rad - 1};
    lv_draw_rect(layer, &r, &a);
  }
}

void refresh_data() {
  const ui::face_data::Snapshot d = ui::face_data::get();
  const int32_t batt = ui::port::battery_percent();
  if (batt != s.last_batt) {
    s.last_batt = batt;
    char b[8];
    ui::face::fmt_battery(b, sizeof(b), batt);
    char buf[16];
    if (s.has_img) {
      std::snprintf(buf, sizeof(buf), "%s%%", b);
      lv_label_set_text(s.batt_v, buf);
    } else {
      std::snprintf(buf, sizeof(buf), "電池 %s%%", b);
      lv_label_set_text(s.batt, buf);
    }
  }
  if (s.has_img && d.steps != s.last_steps) {
    s.last_steps = d.steps;
    char st[12];
    ui::face::fmt_steps(st, sizeof(st), d.steps);
    lv_label_set_text(s.steps_v, st);
  }
}

void tick_time() {
  const Now n = ui::face::now();
  if (n.min_of_day == s.last_min) return;
  s.last_min = n.min_of_day;
  char buf[32];
  if (s.has_img) {
    std::snprintf(buf, sizeof(buf), "%02d", n.hour);
    lv_label_set_text(s.hour, buf);
    std::snprintf(buf, sizeof(buf), "%02d", n.min);
    lv_label_set_text(s.min, buf);
    std::snprintf(buf, sizeof(buf), "%d/%d %s", n.mon, n.mday,
                  ui::face::wd_jp(n.wday));
  } else {
    ui::face::fmt_hm(buf, sizeof(buf), n);
    lv_label_set_text(s.hour, buf);
    ui::face::fmt_date_jp(buf, sizeof(buf), n);
  }
  lv_label_set_text(s.date, buf);
}

lv_obj_t* build(lv_obj_t* scr) {
  const ui::Theme& t = ui::theme();
  lv_obj_set_style_bg_color(scr, t.bg, 0);
  s = S{};

  // 立ち絵: face_chara を優先、無ければ stand (拡大なし)。
  const lv_image_dsc_t* img = t.img_face_chara ? t.img_face_chara
                                             : t.img_stand;
  s.has_img = (img != nullptr);

  if (s.has_img) {
    // 背後のグロー (見本: 右寄り・accent4が中心)。draw イベントで
    // 半径を細かく刻んだ同心円を重ねて輪郭の出ない放射状にする。
    lv_obj_t* glow = lv_obj_create(scr);
    lv_obj_set_size(glow, 410, 502);
    lv_obj_set_pos(glow, 0, 0);
    lv_obj_set_style_bg_opa(glow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(glow, 0, 0);
    lv_obj_remove_flag(glow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(glow, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(glow, draw_glow, LV_EVENT_DRAW_MAIN, nullptr);

    // 立ち絵 (右下)。
    lv_obj_t* im = lv_image_create(scr);
    lv_image_set_src(im, img);
    lv_obj_align(im, LV_ALIGN_BOTTOM_RIGHT, -10, -14);
    lv_obj_add_flag(im, LV_OBJ_FLAG_EVENT_BUBBLE);

    // 左列: 日付 / 時(太) / 短い線 / 分(アクセント)。
    s.date = ui::face::label(scr, "", t.font_body, t.text_dim);
    lv_obj_set_style_text_letter_space(s.date, 2, 0);
    lv_obj_align(s.date, LV_ALIGN_TOP_LEFT, 34, 38);  // 時の ink と重ならない位置

    s.hour = ui::face::label(scr, "00", ui::face::digits(150), t.text);
    lv_obj_align(s.hour, LV_ALIGN_TOP_LEFT, 30, 68);
    lv_obj_set_style_text_letter_space(s.hour, -2, 0);

    // 線の上下に12px以上の余白を取る (時 ink下端 ~222, 分上端 ~254)。
    ui::face::rect(scr, 36, 238, 160 - 36, 3, t.accent5, 1);

    s.min = ui::face::label(scr, "00", ui::face::digits(150), t.accent5);
    lv_obj_align(s.min, LV_ALIGN_TOP_LEFT, 30, 254);
    lv_obj_set_style_text_letter_space(s.min, -2, 0);

    // 左下: 電池・歩数 (名前は薄く、値は白)。
    s.batt = ui::face::label(scr, "電池", t.font_body, t.text_dim);
    lv_obj_align(s.batt, LV_ALIGN_TOP_LEFT, 36, 392);
    s.batt_v = ui::face::label(scr, "--", t.font_body, t.text);
    lv_obj_align(s.batt_v, LV_ALIGN_TOP_LEFT, 92, 392);
    s.steps = ui::face::label(scr, "歩数", t.font_body, t.text_dim);
    lv_obj_align(s.steps, LV_ALIGN_TOP_LEFT, 36, 424);
    s.steps_v = ui::face::label(scr, "--", t.font_body, t.text);
    lv_obj_align(s.steps_v, LV_ALIGN_TOP_LEFT, 92, 424);
  } else {
    // キャラ画像なし: 時刻を中央に置く簡易レイアウト。
    s.hour = ui::face::label(scr, "00:00", ui::face::digits(112), t.text);
    lv_obj_align(s.hour, LV_ALIGN_CENTER, 0, -30);
    s.date = ui::face::label(scr, "", t.font_body, t.text_dim);
    lv_obj_align(s.date, LV_ALIGN_CENTER, 0, 50);
    s.batt = ui::face::label(scr, "", t.font_body, t.text_dim);
    lv_obj_align(s.batt, LV_ALIGN_BOTTOM_MID, 0, -20);
    s.batt_v = lv_label_create(scr);  // 使わないが null 参照を避ける
    s.steps = lv_label_create(scr);
    s.steps_v = lv_label_create(scr);
    for (lv_obj_t* l : {s.batt_v, s.steps, s.steps_v}) {
      lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    }
  }

  tick_time();
  refresh_data();
  return scr;
}

void on_event(const watch::Event& e) {
  switch (e.type) {
    case watch::EventType::ClockTick:
      if (!s.screen_off) {
        tick_time();
        refresh_data();
      }
      break;
    case watch::EventType::BatteryChanged:
    case watch::EventType::ChargingChanged:
    case watch::EventType::BleConnChanged:
      if (!s.screen_off) refresh_data();
      break;
    case watch::EventType::PowerStateChanged:
      s.screen_off = e.arg0 >=
                     static_cast<uint32_t>(watch::PowerState::ScreenOff);
      if (!s.screen_off) {
        s.last_min = -1;
        tick_time();
        refresh_data();
      }
      break;
    default:
      break;
  }
}

}  // namespace

extern const ui::face::Ops kFaceOpsCharaSide = {"chara_side", build, on_event};
