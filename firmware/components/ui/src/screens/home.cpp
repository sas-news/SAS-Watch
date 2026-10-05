// home.cpp — Home 画面: 大きな時刻、日付、電池%、BLE状態。
#include "../components.hpp"
#include "../theme.hpp"

#include <cstdio>
#include "screens.hpp"
#include "ui/port.hpp"
#include "ui/ui.hpp"
#include "watch/features/clock.hpp"
#include "watch/features/timer.hpp"

#include <ctime>

namespace {

struct Home {
  lv_obj_t* time_l = nullptr;
  lv_obj_t* date_l = nullptr;
  lv_obj_t* batt_l = nullptr;
  lv_obj_t* ble_dot = nullptr;
  lv_obj_t* ble_l = nullptr;
  lv_obj_t* timer_chip = nullptr;
  lv_obj_t* timer_l = nullptr;
};
Home s;

void fmt_time(char* buf, size_t cap, int64_t epoch_s, int32_t tz_min) {
  time_t t = static_cast<time_t>(epoch_s + static_cast<int64_t>(tz_min) * 60);
  struct tm tmv {};
  gmtime_r(&t, &tmv);
  std::snprintf(buf, cap, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
}

void fmt_date(char* buf, size_t cap, int64_t epoch_s, int32_t tz_min) {
  static const char* kWd[] = {"日", "月", "火", "水", "木", "金", "土"};
  time_t t = static_cast<time_t>(epoch_s + static_cast<int64_t>(tz_min) * 60);
  struct tm tmv {};
  gmtime_r(&t, &tmv);
  std::snprintf(buf, cap, "%d月%d日 %s曜日", tmv.tm_mon + 1, tmv.tm_mday,
                kWd[tmv.tm_wday]);
}

void refresh() {
  const ui::Ctx& c = ui::ctx();
  const int32_t tz = c.settings ? c.settings->tz_offset_min : 0;
  const int64_t epoch = watch::features::clock_last_epoch_s();
  char buf[64];
  fmt_time(buf, sizeof(buf), epoch, tz);
  lv_label_set_text(s.time_l, buf);
  fmt_date(buf, sizeof(buf), epoch, tz);
  lv_label_set_text(s.date_l, buf);

  const int batt = ui::port::battery_percent();
  if (batt >= 0) {
    std::snprintf(buf, sizeof(buf), "電池 %d%%%s", batt,
                  ui::port::battery_charging() ? " 充電中" : "");
  } else {
    std::snprintf(buf, sizeof(buf), "電池 --%%");
  }
  lv_label_set_text(s.batt_l, buf);
}

void set_timer_chip(bool show) {
  if (show) {
    lv_obj_remove_flag(s.timer_chip, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s.timer_chip, LV_OBJ_FLAG_HIDDEN);
  }
}

void tick_timer_chip() {
  const watch::features::TimerState& ts = watch::features::timer_state();
  if (!ts.running) {
    set_timer_chip(false);
    return;
  }
  set_timer_chip(true);
  const int64_t r = watch::features::timer_remaining_ms(ui::port::now_ms());
  const int total = static_cast<int>(r / 1000);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "タイマー %02d:%02d", total / 60,
                total % 60);
  lv_label_set_text(s.timer_l, buf);
}

lv_obj_t* build(lv_obj_t* scr) {
  const ui::Theme& t = ui::theme();
  lv_obj_set_style_bg_color(scr, t.bg, 0);

  // 時刻
  s.time_l = lv_label_create(scr);
  lv_obj_set_style_text_font(s.time_l, t.font_digits, 0);
  lv_obj_set_style_text_color(s.time_l, t.text, 0);
  lv_label_set_text(s.time_l, "00:00");
  lv_obj_align(s.time_l, LV_ALIGN_CENTER, 0, -96);

  // 日付
  s.date_l = lv_label_create(scr);
  lv_obj_set_style_text_font(s.date_l, t.font_title, 0);
  lv_obj_set_style_text_color(s.date_l, t.text_dim, 0);
  lv_label_set_text(s.date_l, "");
  lv_obj_align(s.date_l, LV_ALIGN_CENTER, 0, -16);

  // タイマー実行中チップ
  s.timer_chip = lv_obj_create(scr);
  lv_obj_set_size(s.timer_chip, 240, 44);
  lv_obj_align(s.timer_chip, LV_ALIGN_CENTER, 0, 48);
  lv_obj_set_style_radius(s.timer_chip, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(s.timer_chip, t.surface2, 0);
  lv_obj_set_style_border_width(s.timer_chip, 0, 0);
  lv_obj_remove_flag(s.timer_chip, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s.timer_chip, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_add_flag(s.timer_chip, LV_OBJ_FLAG_HIDDEN);
  s.timer_l = lv_label_create(s.timer_chip);
  lv_obj_set_style_text_font(s.timer_l, t.font_body, 0);
  lv_obj_set_style_text_color(s.timer_l, t.accent, 0);
  lv_obj_center(s.timer_l);

  // 電池 (下中央)
  s.batt_l = lv_label_create(scr);
  lv_obj_set_style_text_font(s.batt_l, t.font_body, 0);
  lv_obj_set_style_text_color(s.batt_l, t.text_dim, 0);
  lv_obj_align(s.batt_l, LV_ALIGN_BOTTOM_MID, 0, -72);

  // BLE 状態ドット + 文字 (最下部)
  lv_obj_t* row = lv_obj_create(scr);
  lv_obj_set_size(row, 180, 28);
  lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -28);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 0, 0);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
  s.ble_dot = ui::c::status_dot(row, ui::theme().text_dim);
  lv_obj_align(s.ble_dot, LV_ALIGN_LEFT_MID, 18, 0);
  s.ble_l = lv_label_create(row);
  lv_obj_set_style_text_font(s.ble_l, t.font_body, 0);
  lv_obj_set_style_text_color(s.ble_l, t.text_dim, 0);
  lv_label_set_text(s.ble_l, "スマホ未接続");
  lv_obj_align(s.ble_l, LV_ALIGN_LEFT_MID, 42, 0);

  refresh();
  return scr;
}

void on_event(lv_obj_t*, const watch::Event& e) {
  switch (e.type) {
    case watch::EventType::ClockTick:
      refresh();
      tick_timer_chip();
      break;
    case watch::EventType::BatteryChanged:
    case watch::EventType::ChargingChanged:
      refresh();
      break;
    case watch::EventType::BleConnChanged:
      if (e.arg0) {
        ui::c::set_dot(s.ble_dot, ui::theme().ok);
        lv_label_set_text(s.ble_l, "スマホ接続中");
      } else {
        ui::c::set_dot(s.ble_dot, ui::theme().text_dim);
        lv_label_set_text(s.ble_l, "スマホ未接続");
      }
      break;
    case watch::EventType::TimerStarted:
    case watch::EventType::TimerStopped:
      tick_timer_chip();
      break;
    default:
      break;
  }
}

}  // namespace

namespace ui {
extern const ScreenOps kHomeScreen = {watch::Route::Home, build, on_event};
}
