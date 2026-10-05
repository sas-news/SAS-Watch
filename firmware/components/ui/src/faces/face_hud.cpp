// face_hud.cpp — 「hud」文字盤。
//   四隅の L 字飾り線、上段に英字曜日+日付と BLE 状態、中央に大きな数字、
//   和文日付、区切り線、電池/歩数の横バー+数値。秒表示なし。
#include "faces.hpp"

#include <cstdio>

#include "ui/face_data.hpp"
#include "ui/port.hpp"
#include "watch/power.hpp"

namespace {
using ui::face::Now;

struct S {
  lv_obj_t* time = nullptr;
  lv_obj_t* en = nullptr;
  lv_obj_t* jp = nullptr;
  lv_obj_t* ble = nullptr;      // "BLE" ラベル
  lv_obj_t* ble_dot = nullptr;  // 接続ドット
  lv_obj_t* batt_v = nullptr;
  lv_obj_t* batt_fill = nullptr;
  lv_obj_t* steps_v = nullptr;
  lv_obj_t* steps_fill = nullptr;
  int32_t last_min = -1;
  int32_t last_batt = -1;
  int32_t last_steps = -2;
  bool last_ble = false;
  bool screen_off = false;
};
S s;

// 四隅の L 字飾り線 (見本: (30,40)-(70,40)+縦 など、太さ3)。
void draw_corners(lv_event_t* e) {
  lv_layer_t* layer = lv_event_get_layer(e);
  const ui::Theme& t = ui::theme();
  const int kLen = 40;
  const struct { int x, y, sx, sy; } c[4] = {
      {30, 40, 1, 1},    // 左上
      {380, 40, -1, 1},  // 右上
      {30, 462, 1, -1},  // 左下
      {380, 462, -1, -1} // 右下
  };
  for (const auto& cc : c) {
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = t.accent3;
    d.width = 3;
    d.opa = LV_OPA_COVER;
    // 横棒
    d.p1.x = static_cast<float>(cc.x);
    d.p1.y = static_cast<float>(cc.y);
    d.p2.x = static_cast<float>(cc.x + kLen * cc.sx);
    d.p2.y = static_cast<float>(cc.y);
    lv_draw_line(layer, &d);
    // 縦棒
    d.p1.x = static_cast<float>(cc.x);
    d.p1.y = static_cast<float>(cc.y);
    d.p2.x = static_cast<float>(cc.x);
    d.p2.y = static_cast<float>(cc.y + 30 * cc.sy);
    lv_draw_line(layer, &d);
  }
}

void set_bar(lv_obj_t* fill, int32_t pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  lv_obj_set_width(fill, static_cast<lv_coord_t>(2 * pct));
}

void refresh_data() {
  const ui::face_data::Snapshot d = ui::face_data::get();
  const int32_t batt = ui::port::battery_percent();
  if (batt != s.last_batt) {
    s.last_batt = batt;
    set_bar(s.batt_fill, batt < 0 ? 0 : batt);
    char buf[8];
    if (batt < 0) std::snprintf(buf, sizeof(buf), "--%%");
    else std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(batt));
    lv_label_set_text(s.batt_v, buf);
  }
  if (d.steps != s.last_steps) {
    s.last_steps = d.steps;
    set_bar(s.steps_fill,
            (d.steps >= 0 && d.steps_goal > 0)
                ? static_cast<int32_t>(d.steps * 100 / d.steps_goal)
                : 0);
    char buf[12];
    ui::face::fmt_steps(buf, sizeof(buf), d.steps);
    lv_label_set_text(s.steps_v, buf);
  }
}

void tick_time() {
  const Now n = ui::face::now();
  if (n.min_of_day == s.last_min) return;
  s.last_min = n.min_of_day;
  char buf[24];
  ui::face::fmt_hm(buf, sizeof(buf), n);
  lv_label_set_text(s.time, buf);
  std::snprintf(buf, sizeof(buf), "%s %d.%02d", ui::face::wd_en(n.wday),
                n.mon, n.mday);
  lv_label_set_text(s.en, buf);
  ui::face::fmt_date_jp(buf, sizeof(buf), n);
  lv_label_set_text(s.jp, buf);
}

lv_obj_t* build(lv_obj_t* scr) {
  const ui::Theme& t = ui::theme();
  lv_obj_set_style_bg_color(scr, t.bg, 0);
  s = S{};

  // 四隅の L 字 + 区切り線は1つの draw イベント。
  lv_obj_t* deco = lv_obj_create(scr);
  lv_obj_set_size(deco, 410, 502);
  lv_obj_set_pos(deco, 0, 0);
  lv_obj_set_style_bg_opa(deco, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(deco, 0, 0);
  lv_obj_remove_flag(deco, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(deco, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_add_event_cb(deco, draw_corners, LV_EVENT_DRAW_MAIN, nullptr);

  // 上段: 左に EN曜日+日付、右に BLE 状態。
  s.en = ui::face::label(scr, "--- --.--", t.font_body, t.accent3);
  lv_obj_set_style_text_letter_space(s.en, 3, 0);
  lv_obj_align(s.en, LV_ALIGN_TOP_LEFT, 44, 56);

  s.ble = ui::face::label(scr, "BLE", t.font_body, t.text_dim);
  lv_obj_align(s.ble, LV_ALIGN_TOP_RIGHT, -76, 56);
  s.ble_dot = lv_obj_create(scr);
  lv_obj_set_size(s.ble_dot, 12, 12);
  lv_obj_set_style_radius(s.ble_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(s.ble_dot, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s.ble_dot, 0, 0);
  lv_obj_align(s.ble_dot, LV_ALIGN_TOP_RIGHT, -50, 62);
  lv_obj_set_style_bg_color(s.ble_dot, t.text_dim, 0);
  lv_obj_add_flag(s.ble_dot, LV_OBJ_FLAG_EVENT_BUBBLE);

  // 中央の大きな数字。
  s.time = ui::face::label(scr, "00:00", ui::face::digits(112), t.text);
  lv_obj_align(s.time, LV_ALIGN_TOP_MID, 0, 96);
  lv_obj_set_style_text_letter_space(s.time, 2, 0);

  // 和文日付 (左寄せ)。
  s.jp = ui::face::label(scr, "", t.font_body, t.text_dim);
  lv_obj_align(s.jp, LV_ALIGN_TOP_LEFT, 56, 238);

  // 区切り線。
  ui::face::rect(scr, 40, 300, 370 - 40, 2,
                 lv_color_mix(t.accent3, t.bg, 30), 0);

  // 電池バー + 数値 (digit font は % 非収録なので値は font_body)。
  lv_obj_t* bl = ui::face::label(scr, "電池", t.font_body, t.text_dim);
  lv_obj_align(bl, LV_ALIGN_TOP_LEFT, 60, 326);
  ui::face::rect(scr, 60, 356, 200, 10,
                 lv_color_mix(t.accent3, t.bg, 46), 5);  // トラック
  s.batt_fill = ui::face::rect(scr, 60, 356, 0, 10, t.accent3, 5);
  s.batt_v = ui::face::label(scr, "--", t.font_body, t.text);
  lv_obj_align(s.batt_v, LV_ALIGN_TOP_LEFT, 276, 350);

  // 歩数バー + 数値。
  lv_obj_t* sl = ui::face::label(scr, "歩数", t.font_body, t.text_dim);
  lv_obj_align(sl, LV_ALIGN_TOP_LEFT, 60, 376);
  ui::face::rect(scr, 60, 406, 200, 10,
                 lv_color_mix(t.accent, t.bg, 46), 5);
  s.steps_fill = ui::face::rect(scr, 60, 406, 0, 10, t.accent, 5);
  s.steps_v = ui::face::label(scr, "--", t.font_body, t.text);
  lv_obj_align(s.steps_v, LV_ALIGN_TOP_LEFT, 276, 400);

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
      if (!s.screen_off) refresh_data();
      break;
    case watch::EventType::BleConnChanged:
      if (!s.screen_off) {
        const bool on = e.arg0 != 0;
        lv_obj_set_style_bg_color(s.ble_dot,
                                  on ? ui::theme().ok : ui::theme().text_dim,
                                  0);
      }
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

extern const ui::face::Ops kFaceOpsHud = {"hud", build, on_event};
