// face_minimal.cpp — 「minimal」文字盤。
//   細い数字、1日の進捗を表す270度の弧 (2色の段階グラデ + 端点の白丸)、
//   日付、次のアラーム、電池・歩数。
//   弧は LV_EVENT_DRAW_MAIN で描き、進捗が変わる分の更新のみ invalidate。
#include "faces.hpp"

#include <cmath>
#include <cstdio>

#include "ui/face_data.hpp"
#include "ui/port.hpp"
#include "watch/power.hpp"

namespace {
using ui::face::Now;

// 弧の中心・半径 (見本: 中心 (205,170), r160, 下端に135°の切れ目)。
constexpr int kCx = 205;
constexpr int kCy = 170;
constexpr int kR = 160;
// LVGL 角 (0=3時、時計回り)。切れ目が下中央になるよう 135°→45° で270°。
constexpr float kStart = 135.f;
constexpr float kSweep = 270.f;

struct S {
  lv_obj_t* dial = nullptr;
  lv_obj_t* time = nullptr;
  lv_obj_t* date = nullptr;
  lv_obj_t* alarm = nullptr;
  lv_obj_t* info = nullptr;
  int32_t last_min = -1;
  int32_t last_batt = -1;
  int32_t last_steps = -2;
  int32_t last_alarm = -2;
  bool screen_off = false;
};
S s;

void draw_face(lv_event_t* e) {
  lv_layer_t* layer = lv_event_get_layer(e);
  const ui::Theme& t = ui::theme();

  // トラック (270°、切れ目が下)。
  lv_draw_arc_dsc_t a;
  lv_draw_arc_dsc_init(&a);
  a.center.x = kCx;
  a.center.y = kCy;
  a.radius = kR;
  a.width = 10;
  a.rounded = 1;
  a.color = t.surface2;
  a.opa = LV_OPA_COVER;
  a.start_angle = static_cast<lv_value_precise_t>(kStart);
  a.end_angle = static_cast<lv_value_precise_t>(kStart + kSweep);
  lv_draw_arc(layer, &a);

  // 進捗 (0-1440分 → 270°)。LVGL での帯グラデが難しいので
  // 前半 accent4 / 後半 accent5 の2色で見本に寄せる。
  const float prog = s.last_min < 0 ? 0.f
                                    : static_cast<float>(s.last_min) / 1440.f;
  const float sweep = kSweep * prog;
  const float half = kSweep * 0.5f;
  a.color = t.accent4;
  a.start_angle = static_cast<lv_value_precise_t>(kStart);
  a.end_angle =
      static_cast<lv_value_precise_t>(kStart + (sweep < half ? sweep : half));
  if (sweep > 0.5f) lv_draw_arc(layer, &a);
  if (sweep > half) {
    a.color = t.accent5;
    a.start_angle = static_cast<lv_value_precise_t>(kStart + half);
    a.end_angle = static_cast<lv_value_precise_t>(kStart + sweep);
    lv_draw_arc(layer, &a);
  }
  // 端点の白丸。
  if (sweep > 0.5f) {
    const float rad = (kStart + sweep) * 0.0174532925f;
    const float dx = kCx + kR * cosf(rad);
    const float dy = kCy + kR * sinf(rad);
    lv_draw_arc_dsc_t dot;
    lv_draw_arc_dsc_init(&dot);
    dot.center.x = static_cast<lv_coord_t>(dx);
    dot.center.y = static_cast<lv_coord_t>(dy);
    dot.radius = 9;
    dot.width = 9;
    dot.start_angle = 0;
    dot.end_angle = 360;
    dot.color = t.text;
    dot.opa = LV_OPA_COVER;
    lv_draw_arc(layer, &dot);
  }
}

void refresh_data() {
  const ui::face_data::Snapshot d = ui::face_data::get();
  const int32_t batt = ui::port::battery_percent();
  bool dirty = false;
  if (batt != s.last_batt || d.steps != s.last_steps) {
    s.last_batt = batt;
    s.last_steps = d.steps;
    char b[8], st[12];
    ui::face::fmt_battery(b, sizeof(b), batt);
    ui::face::fmt_steps(st, sizeof(st), d.steps);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "電池 %s%%・%s 歩", b, st);
    lv_label_set_text(s.info, buf);
    dirty = true;
  }
  if (d.next_alarm_min != s.last_alarm) {
    s.last_alarm = d.next_alarm_min;
    char buf[40];
    if (d.next_alarm_min >= 0) {
      std::snprintf(buf, sizeof(buf), "次の予定　%02d:%02d アラーム",
                    static_cast<int>(d.next_alarm_min / 60),
                    static_cast<int>(d.next_alarm_min % 60));
    } else {
      std::snprintf(buf, sizeof(buf), "次の予定　--:--");
    }
    lv_label_set_text(s.alarm, buf);
    dirty = true;
  }
  (void)dirty;
}

void tick_time() {
  const Now n = ui::face::now();
  if (n.min_of_day == s.last_min) return;
  s.last_min = n.min_of_day;
  char buf[24];
  ui::face::fmt_hm(buf, sizeof(buf), n);
  lv_label_set_text(s.time, buf);
  ui::face::fmt_date_jp(buf, sizeof(buf), n);
  lv_label_set_text(s.date, buf);
  lv_obj_invalidate(s.dial);  // 弧の進捗も分単位でだけ更新
}

lv_obj_t* build(lv_obj_t* scr) {
  const ui::Theme& t = ui::theme();
  lv_obj_set_style_bg_color(scr, t.bg, 0);
  s = S{};

  // 270°の弧 (日の進捗) — draw イベントで1回に描く。
  s.dial = lv_obj_create(scr);
  lv_obj_set_size(s.dial, 410, 502);
  lv_obj_set_pos(s.dial, 0, 0);
  lv_obj_set_style_bg_opa(s.dial, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(s.dial, 0, 0);
  lv_obj_remove_flag(s.dial, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s.dial, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_add_event_cb(s.dial, draw_face, LV_EVENT_DRAW_MAIN, nullptr);

  // 中央の細い数字。
  s.time = ui::face::label(scr, "00:00", ui::face::digits(112), t.text);
  lv_obj_align(s.time, LV_ALIGN_TOP_MID, 0, 148);
  lv_obj_set_style_text_letter_space(s.time, -3, 0);

  // 日付。
  s.date = ui::face::label(scr, "", t.font_body, t.text_dim);
  lv_obj_align(s.date, LV_ALIGN_TOP_MID, 0, 296);

  // 次のアラーム。
  s.alarm = ui::face::label(scr, "", t.font_body, t.text_dim);
  lv_obj_align(s.alarm, LV_ALIGN_TOP_MID, 0, 400);

  // 電池・歩数。
  s.info = ui::face::label(scr, "", t.font_body, t.text_dim);
  lv_obj_align(s.info, LV_ALIGN_TOP_MID, 0, 440);

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

extern const ui::face::Ops kFaceOpsMinimal = {"minimal", build, on_event};
