// face_analog.cpp — 「analog」文字盤。
//   放射グラデの盤面、60目盛り (5の倍数は太く明るい)、12/3/6/9、
//   日付小窓、ロゴ、時針(太)・分針、中心キャップ。秒針なし。
//   下に「電池 87%  ·  6,214 歩」。
//   目盛り・針は1つの draw イベント (LV_EVENT_DRAW_MAIN) にまとめる。
//   針の更新は分が変わった時だけ。
#include "faces.hpp"

#include <cmath>
#include <cstdio>

#include "ui/face_data.hpp"
#include "ui/port.hpp"
#include "watch/power.hpp"

namespace {
using ui::face::Now;

// 盤面の中心・半径 (見本どおり)。
constexpr int kCx = 205;
constexpr int kCy = 240;
constexpr int kDialR = 190;

struct S {
  lv_obj_t* dial = nullptr;   // 目盛り+針+キャップを draw するオブジェクト
  lv_obj_t* date_l = nullptr; // 日付小窓の数字
  lv_obj_t* info = nullptr;   // 下部の電池・歩数
  lv_obj_t* logo = nullptr;
  Now cur;
  int32_t last_min = -1;
  int32_t last_batt = -1;
  int32_t last_steps = -2;
  bool screen_off = false;
};
S s;

lv_grad_dsc_t s_grad;  // bg_grad は静的に持つ (style が参照する)

inline void polar(int deg_from_12, float r, lv_value_precise_t* x,
                  lv_value_precise_t* y) {
  const float rad = static_cast<float>(deg_from_12) * 0.0174532925f;
  *x = kCx + static_cast<lv_value_precise_t>(lroundf(r * sinf(rad)));
  *y = kCy - static_cast<lv_value_precise_t>(lroundf(r * cosf(rad)));
}

// 目盛り + 針 + キャップを1度の描画イベントで。
void draw_dial(lv_event_t* e) {
  lv_layer_t* layer = lv_event_get_layer(e);
  const ui::Theme& t = ui::theme();

  // ---- 60目盛り ----
  for (int i = 0; i < 60; ++i) {
    const bool big = (i % 5) == 0;
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = big ? t.accent2 : lv_color_mix(t.accent2, t.bg, 60);
    d.width = big ? 4 : 2;
    d.opa = LV_OPA_COVER;
    polar(i * 6, big ? 168.f : 176.f, &d.p1.x, &d.p1.y);
    polar(i * 6, 186.f, &d.p2.x, &d.p2.y);
    lv_draw_line(layer, &d);
  }

  // ---- 針 (角度は 12時=0°、時計回り) ----
  const float hdeg = (s.cur.hour % 12) * 30.f + s.cur.min * 0.5f;
  const float mdeg = s.cur.min * 6.f;
  lv_draw_line_dsc_t d;
  lv_draw_line_dsc_init(&d);
  d.round_start = 1;
  d.round_end = 1;
  // 時針 (太・短)
  d.color = t.accent2;
  d.width = 10;
  d.opa = LV_OPA_COVER;
  polar(hdeg, -18.f, &d.p1.x, &d.p1.y);   // 中心から少し逆行した尻尾
  polar(hdeg, 100.f, &d.p2.x, &d.p2.y);
  lv_draw_line(layer, &d);
  // 分針 (細・長・明色)
  d.color = t.text;
  d.width = 6;
  polar(mdeg, -22.f, &d.p1.x, &d.p1.y);
  polar(mdeg, 150.f, &d.p2.x, &d.p2.y);
  lv_draw_line(layer, &d);
  // 中心キャップ (外側アクセント + 中の抜き穴)
  lv_draw_arc_dsc_t cap;
  lv_draw_arc_dsc_init(&cap);
  cap.center.x = kCx;
  cap.center.y = kCy;
  cap.radius = 9;
  cap.width = 9;  // 内側まで埋める
  cap.start_angle = 0;
  cap.end_angle = 360;
  cap.color = t.accent;
  cap.opa = LV_OPA_COVER;
  lv_draw_arc(layer, &cap);
  cap.radius = 3;
  cap.width = 3;
  cap.color = t.bg;
  lv_draw_arc(layer, &cap);
}

void refresh_data() {
  const ui::face_data::Snapshot d = ui::face_data::get();
  const int32_t batt = ui::port::battery_percent();
  const int32_t steps = d.steps;
  if (batt == s.last_batt && steps == s.last_steps) return;
  s.last_batt = batt;
  s.last_steps = steps;
  char b[8], st[12];
  ui::face::fmt_battery(b, sizeof(b), batt);
  ui::face::fmt_steps(st, sizeof(st), steps);
  char buf[48];
  std::snprintf(buf, sizeof(buf), "電池 %s%%・%s 歩", b, st);
  lv_label_set_text(s.info, buf);
}

void tick_time() {
  const Now n = ui::face::now();
  if (n.min_of_day == s.last_min) return;
  s.last_min = n.min_of_day;
  s.cur = n;
  char buf[4];
  std::snprintf(buf, sizeof(buf), "%d", n.mday);
  lv_label_set_text(s.date_l, buf);
  lv_obj_invalidate(s.dial);  // 針だけでなく盤面をまとめて再描画
}

lv_obj_t* build(lv_obj_t* scr) {
  const ui::Theme& t = ui::theme();
  lv_obj_set_style_bg_color(scr, t.bg, 0);
  s = S{};

  // 盤面の放射グラデーション (見本: 中心 #14181f → 外 #000)。
  // grad の座標系はこのオブジェクトのローカル座標。
  {
    const lv_color_t cols[2] = {lv_color_mix(t.text_dim, t.bg, 28), t.bg};
    const lv_opa_t opas[2] = {LV_OPA_COVER, LV_OPA_COVER};
    const uint8_t fr[2] = {0, 255};
    lv_grad_init_stops(&s_grad, cols, opas, fr, 2);
    lv_grad_radial_init(&s_grad, kDialR + 10, kDialR + 10,
                        kDialR + 10 + kDialR - 8, kDialR + 10,
                        LV_GRAD_EXTEND_PAD);
  }
  lv_obj_t* disc = lv_obj_create(scr);
  lv_obj_set_size(disc, kDialR * 2 + 20, kDialR * 2 + 20);
  lv_obj_align(disc, LV_ALIGN_CENTER, kCx - 205, kCy - 251);
  lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(disc, t.bg, 0);
  lv_obj_set_style_bg_grad(disc, &s_grad, 0);
  lv_obj_set_style_border_width(disc, 0, 0);
  lv_obj_remove_flag(disc, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(disc, LV_OBJ_FLAG_EVENT_BUBBLE);

  // 目盛り + 針 + キャップは1つの draw イベントにまとめる。
  s.dial = lv_obj_create(scr);
  lv_obj_set_size(s.dial, 410, 502);
  lv_obj_set_pos(s.dial, 0, 0);
  lv_obj_set_style_bg_opa(s.dial, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(s.dial, 0, 0);
  lv_obj_remove_flag(s.dial, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s.dial, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_add_event_cb(s.dial, draw_dial, LV_EVENT_DRAW_MAIN, nullptr);

  // 数字 12/3/6/9 (盤内側)。
  const lv_font_t* fn = ui::face::digits(34);
  const int num_pos[4][2] = {
      {205, 88},   // 12
      {358, 236},  // 3
      {205, 384},  // 6
      {52, 236},   // 9
  };
  const char* num_txt[4] = {"12", "3", "6", "9"};
  for (int i = 0; i < 4; ++i) {
    lv_obj_t* l = ui::face::label(scr, num_txt[i], fn, t.accent2);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_pos(l, num_pos[i][0] - lv_obj_get_width(l) / 2,
                   num_pos[i][1] - lv_obj_get_height(l) / 2);
  }

  // 日付小窓 (3時位置の内側)。
  lv_obj_t* win = ui::face::rect(scr, 272, 226, 40, 28, t.surface2,
                                 t.radius_sm / 2);
  lv_obj_set_style_border_width(win, 1, 0);
  lv_obj_set_style_border_color(win, lv_color_mix(t.text_dim, t.bg, 80), 0);
  s.date_l = ui::face::label(win, "-", ui::face::digits(18), t.text);
  lv_obj_center(s.date_l);

  // ロゴ。
  s.logo = ui::face::label(scr, "SAS WATCH", t.font_body, t.text_dim);
  lv_obj_set_style_text_letter_space(s.logo, 4, 0);
  lv_obj_align(s.logo, LV_ALIGN_TOP_MID, 0, 148);

  // 下部の電池・歩数行。
  s.info = ui::face::label(scr, "", t.font_body, t.text_dim);
  lv_obj_align(s.info, LV_ALIGN_BOTTOM_MID, 0, -18);

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

extern const ui::face::Ops kFaceOpsAnalog = {"analog", build, on_event};
