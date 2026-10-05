// timer.cpp — タイマー: 分秒の設定・開始/停止/リセット。
// 終了時のフルスクリーン通知は shell.cpp のアラート。
#include "../components.hpp"
#include "../theme.hpp"

#include <cstdio>
#include "screens.hpp"
#include "ui/port.hpp"
#include "ui/ui.hpp"
#include "watch/features/timer.hpp"

namespace {

struct T {
  lv_obj_t* setup_box = nullptr;
  lv_obj_t* run_box = nullptr;
  lv_obj_t* setup_l = nullptr;
  lv_obj_t* remain_l = nullptr;
  lv_timer_t* tick = nullptr;
  uint32_t setup_s = 60;
};
T s;

void fmt_mmss(char* buf, size_t cap, int64_t ms) {
  const int total = static_cast<int>((ms + 500) / 1000);
  std::snprintf(buf, cap, "%02d:%02d", total / 60, total % 60);
}

void show_setup(bool setup) {
  if (setup) {
    lv_obj_remove_flag(s.setup_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s.run_box, LV_OBJ_FLAG_HIDDEN);
    char buf[16];
    fmt_mmss(buf, sizeof(buf), static_cast<int64_t>(s.setup_s) * 1000);
    lv_label_set_text(s.setup_l, buf);
  } else {
    lv_obj_add_flag(s.setup_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s.run_box, LV_OBJ_FLAG_HIDDEN);
  }
}

void tick_cb(lv_timer_t*) {
  const int64_t r =
      watch::features::timer_remaining_ms(ui::port::now_ms());
  char buf[16];
  fmt_mmss(buf, sizeof(buf), r);
  lv_label_set_text(s.remain_l, buf);
}

void sync_view() {
  const bool running = watch::features::timer_state().running;
  show_setup(!running);
  if (running) {
    if (!s.tick) s.tick = lv_timer_create(tick_cb, 250, nullptr);
    tick_cb(nullptr);
  } else if (s.tick) {
    lv_timer_delete(s.tick);
    s.tick = nullptr;
  }
}

void adj(int32_t ds) {
  int32_t v = static_cast<int32_t>(s.setup_s) + ds;
  if (v < 5) v = 5;
  if (v > 3600) v = 3600;
  s.setup_s = static_cast<uint32_t>(v);
  show_setup(true);
}

lv_obj_t* build(lv_obj_t* scr) {
  const ui::Theme& t = ui::theme();
  lv_obj_set_style_bg_color(scr, t.bg, 0);
  ui::c::header(scr, "タイマー", true);
  lv_obj_t* col = ui::c::content(scr);
  s = T{};  // 画面再作成で状態リセット (setup_s も既定に)
  s.setup_s = watch::features::timer_state().duration_s;

  // --- 設定ビュー ---
  s.setup_box = ui::c::card(col);
  lv_obj_set_flex_align(s.setup_box, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  s.setup_l = lv_label_create(s.setup_box);
  lv_obj_set_style_text_font(s.setup_l, t.font_digits_sm, 0);
  lv_obj_set_style_text_color(s.setup_l, t.text, 0);

  ui::c::button(s.setup_box, "＋1分", [](lv_event_t*) { adj(60); }, nullptr);
  ui::c::button(s.setup_box, "＋10秒", [](lv_event_t*) { adj(10); }, nullptr);
  ui::c::button(s.setup_box, "−10秒", [](lv_event_t*) { adj(-10); }, nullptr);
  ui::c::button_primary(
      s.setup_box, "開始",
      [](lv_event_t*) {
        ui::emit(watch::ActionType::TimerStart, s.setup_s);
      },
      nullptr);

  // --- 実行中ビュー ---
  s.run_box = ui::c::card(col);
  lv_obj_set_flex_align(s.run_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  s.remain_l = lv_label_create(s.run_box);
  lv_obj_set_style_text_font(s.remain_l, t.font_digits_sm, 0);
  lv_obj_set_style_text_color(s.remain_l, t.accent, 0);
  lv_label_set_text(s.remain_l, "00:00");

  ui::c::button(s.run_box, "停止",
                [](lv_event_t*) { ui::emit(watch::ActionType::TimerStop); },
                nullptr);
  ui::c::button(s.run_box, "リセット",
                [](lv_event_t*) { ui::emit(watch::ActionType::TimerReset); },
                nullptr);

  sync_view();
  return scr;
}

void on_event(lv_obj_t*, const watch::Event& e) {
  switch (e.type) {
    case watch::EventType::TimerStarted:
    case watch::EventType::TimerStopped:
    case watch::EventType::TimerFinished:
      sync_view();
      break;
    default:
      break;
  }
}

}  // namespace

namespace ui {
extern const ScreenOps kTimerScreen = {watch::Route::Timer, build, on_event};
}
