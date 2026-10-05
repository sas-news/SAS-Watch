// face_bold.cpp — 「bold」文字盤。
//   上: 日付 (曜日だけアクセント色)。中央: 時 (太) / 分 (細・アクセント)。
//   下: 電池・歩数・通知のリングメーター3個 (数値+ラベル)。
//   時刻は分が変わった時だけ、リングは値が変わった時だけ更新する。
#include "faces.hpp"

#include <cstdio>

#include "../components.hpp"
#include "ui/face_data.hpp"
#include "ui/port.hpp"
#include "watch/power.hpp"

namespace {
using ui::face::Now;

constexpr int kRingCx[3] = {105, 205, 305};
constexpr int kRingCy = 440;
// 通知リングの満杯カウント (8通で 100%)。
constexpr int kNotifMax = 8;

struct S {
  lv_obj_t* date = nullptr;
  lv_obj_t* wday = nullptr;
  lv_obj_t* hour = nullptr;
  lv_obj_t* min = nullptr;
  lv_obj_t* ring[3] = {};
  lv_obj_t* rval[3] = {};
  int32_t last_min = -1;
  int32_t last_batt = -1;
  int32_t last_steps = -2;
  int32_t last_notif = -2;
  bool screen_off = false;
};
S s;

void set_ring(int i, int32_t pct, const char* txt) {
  lv_arc_set_value(s.ring[i], static_cast<int32_t>(pct));
  lv_label_set_text(s.rval[i], txt);
}

// 値が変わった時だけ描く。
void refresh_data() {
  const ui::face_data::Snapshot d = ui::face_data::get();
  const int32_t batt = ui::port::battery_percent();
  if (batt != s.last_batt) {
    s.last_batt = batt;
    char buf[8];
    ui::face::fmt_battery(buf, sizeof(buf), batt);
    set_ring(0, batt < 0 ? 0 : batt, buf);
  }
  if (d.steps != s.last_steps) {
    s.last_steps = d.steps;
    char buf[8];
    if (d.steps < 0) {
      set_ring(1, 0, "--");
    } else if (d.steps < 1000) {
      std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(d.steps));
      set_ring(1, d.steps_goal > 0 ? (d.steps * 100) / d.steps_goal : 0, buf);
    } else {
      std::snprintf(buf, sizeof(buf), "%d.%dk",
                    static_cast<int>(d.steps / 1000),
                    static_cast<int>((d.steps % 1000) / 100));
      set_ring(1, d.steps_goal > 0 ? (d.steps * 100) / d.steps_goal : 0, buf);
    }
  }
  if (d.notifications != s.last_notif) {
    s.last_notif = d.notifications;
    const int32_t n = d.notifications < 0 ? 0 : d.notifications;
    char buf[4];
    ui::face::fmt_battery(buf, sizeof(buf), d.notifications);
    set_ring(2, n > kNotifMax ? 100 : n * 100 / kNotifMax, buf);
  }
}

void tick_time() {
  const Now n = ui::face::now();
  if (n.min_of_day == s.last_min) return;
  s.last_min = n.min_of_day;
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%02d", n.hour);
  lv_label_set_text(s.hour, buf);
  std::snprintf(buf, sizeof(buf), "%02d", n.min);
  lv_label_set_text(s.min, buf);
  std::snprintf(buf, sizeof(buf), "%d月%d日", n.mon, n.mday);
  lv_label_set_text(s.date, buf);
  lv_label_set_text(s.wday, ui::face::wd_jp(n.wday));
}

lv_obj_t* build(lv_obj_t* scr) {
  const ui::Theme& t = ui::theme();
  lv_obj_set_style_bg_color(scr, t.bg, 0);
  s = S{};

  // 日付 (曜日だけアクセント色にしたいので flex 行で2ラベル)。
  lv_obj_t* row = lv_obj_create(scr);
  lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 34);
  lv_obj_set_layout(row, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 6, 0);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 0, 0);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
  s.date = ui::face::label(row, "", t.font_body, t.text_dim);
  s.wday = ui::face::label(row, "", t.font_body, t.accent);

  // 時 (太) / 分 (細・アクセント色)。
  s.hour = ui::face::label(scr, "00", ui::face::digits(150), t.text);
  lv_obj_align(s.hour, LV_ALIGN_TOP_MID, 0, 62);
  lv_obj_set_style_text_letter_space(s.hour, -2, 0);
  s.min = ui::face::label(scr, "00", ui::face::digits_thin(150), t.accent);
  lv_obj_align(s.min, LV_ALIGN_TOP_MID, 0, 214);
  lv_obj_set_style_text_letter_space(s.min, -2, 0);

  // 間の細線。
  ui::face::rect(scr, 60, 212, 350 - 60, 2, t.surface2, 0);

  // リングメーター3個: 電池/歩数/通知。
  const lv_color_t ring_col[3] = {t.ok, t.accent, t.primary};
  const char* ring_name[3] = {"電池", "歩数", "通知"};
  for (int i = 0; i < 3; ++i) {
    lv_obj_t* a = lv_arc_create(scr);
    lv_obj_set_size(a, 60, 60);
    lv_obj_align(a, LV_ALIGN_TOP_LEFT, kRingCx[i] - 30, kRingCy - 30);
    lv_arc_set_bg_angles(a, 0, 360);
    lv_arc_set_range(a, 0, 100);
    lv_arc_set_rotation(a, 270);  // 上から時計回り
    lv_arc_set_value(a, 0);
    lv_obj_set_style_arc_color(a, t.surface2, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, ring_col[i], LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(a, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
    lv_obj_set_style_opa(a, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(a, LV_OBJ_FLAG_EVENT_BUBBLE);
    s.ring[i] = a;

    // digit font は % . k 非収録なので値は font_body。
    s.rval[i] = ui::face::label(a, "--", t.font_body, t.text);
    lv_obj_center(s.rval[i]);

    lv_obj_t* nm = ui::face::label(scr, ring_name[i], t.font_body,
                                   t.text_dim);
    lv_obj_align(nm, LV_ALIGN_TOP_MID, kRingCx[i] - 205, 476);
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
        refresh_data();  // 変化時のみ内部で早期return
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

extern const ui::face::Ops kFaceOpsBold = {"bold", build, on_event};
