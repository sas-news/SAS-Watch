// timer.cpp — タイマー: 分秒の設定・開始/一時停止/リセット。
// 実行中は残り時間のリング (lv_arc) + 時計フォントの大きな数字。
// 終了時のフルスクリーン通知は shell.cpp のアラート。
#include "../components.hpp"
#include "../faces/faces.hpp"
#include "../theme.hpp"

#include <cstdio>
#include "screens.hpp"
#include "ui/port.hpp"
#include "ui/ui.hpp"
#include "watch/features/timer.hpp"

namespace {

struct T {
  lv_obj_t* setup_box = nullptr;
  lv_obj_t* run_wrap = nullptr;
  lv_obj_t* setup_l = nullptr;
  lv_obj_t* arc = nullptr;
  lv_obj_t* remain_l = nullptr;
  lv_obj_t* cap_l = nullptr;
  lv_obj_t* pause_l = nullptr;  // 一時停止/再開ボタンのラベル
  lv_timer_t* tick = nullptr;
  uint32_t setup_s = 60;
};
T s;

void fmt_mmss(char* buf, size_t cap, int64_t ms) {
  const int total = static_cast<int>((ms + 500) / 1000);
  std::snprintf(buf, cap, "%02d:%02d", total / 60, total % 60);
}

void fmt_ms(char* buf, size_t cap, uint32_t sec) {
  std::snprintf(buf, cap, "%u:%02u", sec / 60, sec % 60);
}

void show_setup(bool setup) {
  if (setup) {
    lv_obj_remove_flag(s.setup_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s.run_wrap, LV_OBJ_FLAG_HIDDEN);
    char buf[16];
    fmt_mmss(buf, sizeof(buf), static_cast<int64_t>(s.setup_s) * 1000);
    lv_label_set_text(s.setup_l, buf);
  } else {
    lv_obj_add_flag(s.setup_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s.run_wrap, LV_OBJ_FLAG_HIDDEN);
  }
}

void tick_cb(lv_timer_t*) {
  const watch::features::TimerState& st = watch::features::timer_state();
  const int64_t r =
      watch::features::timer_remaining_ms(ui::port::now_ms());
  char buf[24];
  fmt_mmss(buf, sizeof(buf), r);
  lv_label_set_text(s.remain_l, buf);

  // リングは残り割合。上端 (270°) から時計回りに 360*frac。
  const int64_t total = static_cast<int64_t>(st.duration_s) * 1000;
  const int frac = total > 0 ? static_cast<int>((r * 360) / total) : 0;
  lv_arc_set_angles(s.arc, 270, 270 + frac);

  if (st.paused) {
    lv_label_set_text(s.cap_l, "一時停止中");
    lv_label_set_text(s.pause_l, "再開");
  } else {
    char dur[16];
    fmt_ms(dur, sizeof(dur), st.duration_s);
    lv_label_set_text_fmt(s.cap_l, "%s のうち", dur);
    // ↑ duration は uint32_t 最大 3599 → fmt_ms 出力は最大 "59:99" (5ch)。
    //   buf(24) は十分。snprintf だと -Wformat-truncation が出るので
    //   lv_label_set_text_fmt を使う。
    lv_label_set_text(s.pause_l, "一時停止");
  }
}

void sync_view() {
  const watch::features::TimerState& st = watch::features::timer_state();
  const bool run = st.running || st.paused;
  show_setup(!run);
  if (run) {
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

  // --- 設定ビュー (大きな数字 + 調整/開始ボタン) ---
  s.setup_box = ui::c::card(col);
  lv_obj_set_flex_align(s.setup_box, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  s.setup_l = lv_label_create(s.setup_box);
  lv_obj_add_flag(s.setup_l, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_set_style_text_font(s.setup_l, ui::face::digits(112), 0);
  lv_obj_set_style_text_color(s.setup_l, t.text, 0);

  ui::c::button(s.setup_box, "＋1分", [](lv_event_t*) { adj(60); }, nullptr);
  ui::c::button(s.setup_box, "＋10秒", [](lv_event_t*) { adj(10); }, nullptr);
  ui::c::button(s.setup_box, "−10秒", [](lv_event_t*) { adj(-10); },
                nullptr);
  ui::c::button_primary(
      s.setup_box, "開始",
      [](lv_event_t*) {
        ui::emit(watch::ActionType::TimerStart, s.setup_s);
      },
      nullptr);

  // --- 実行中ビュー (リング + 数字 + キャプション + ボタン) ---
  s.run_wrap = lv_obj_create(col);
  lv_obj_add_flag(s.run_wrap, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_set_size(s.run_wrap, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(s.run_wrap, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(s.run_wrap, 0, 0);
  lv_obj_set_style_pad_all(s.run_wrap, 0, 0);
  lv_obj_set_style_pad_row(s.run_wrap, 12, 0);
  lv_obj_set_flex_flow(s.run_wrap, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s.run_wrap, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
  lv_obj_remove_flag(s.run_wrap, LV_OBJ_FLAG_SCROLLABLE);

  // リング領域 (絶対配置で数字とキャプションを円内に重ねる)。
  lv_obj_t* ring = lv_obj_create(s.run_wrap);
  lv_obj_add_flag(ring, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_set_size(ring, LV_PCT(100), 272);
  lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(ring, 0, 0);
  lv_obj_set_style_pad_all(ring, 0, 0);
  lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);

  // 残り時間リング (太さ 14・丸端)。バックは全周、インジケータが残り。
  s.arc = lv_arc_create(ring);
  lv_obj_add_flag(s.arc, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_remove_flag(s.arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(s.arc, 260, 260);
  lv_obj_align(s.arc, LV_ALIGN_TOP_MID, 0, 6);
  lv_arc_set_bg_angles(s.arc, 0, 360);
  lv_arc_set_angles(s.arc, 270, 270);
  lv_obj_set_style_arc_width(s.arc, 14, LV_PART_MAIN);
  lv_obj_set_style_arc_width(s.arc, 14, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(s.arc, t.surface2, LV_PART_MAIN);
  lv_obj_set_style_arc_color(s.arc, t.primary, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(s.arc, true, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(s.arc, true, LV_PART_INDICATOR);
  lv_obj_set_style_opa(s.arc, LV_OPA_TRANSP, LV_PART_KNOB);

  s.remain_l = lv_label_create(ring);
  lv_obj_add_flag(s.remain_l, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_label_set_text(s.remain_l, "00:00");
  lv_obj_set_style_text_font(s.remain_l, ui::face::digits(112), 0);
  lv_obj_set_style_text_color(s.remain_l, t.text, 0);
  lv_obj_align(s.remain_l, LV_ALIGN_CENTER, 0, -12);

  s.cap_l = lv_label_create(ring);
  lv_obj_add_flag(s.cap_l, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_label_set_text(s.cap_l, "");
  lv_obj_set_style_text_font(s.cap_l, t.font_body, 0);
  lv_obj_set_style_text_color(s.cap_l, t.text_dim, 0);
  lv_obj_align(s.cap_l, LV_ALIGN_CENTER, 0, 62);

  lv_obj_t* pb = ui::c::button_primary(
      s.run_wrap, "一時停止",
      [](lv_event_t*) {
        const watch::features::TimerState& st =
            watch::features::timer_state();
        if (st.paused) {
          ui::emit(watch::ActionType::TimerStart, 0);  // 再開
        } else {
          ui::emit(watch::ActionType::TimerPause);
        }
      },
      nullptr);
  s.pause_l = lv_obj_get_child(pb, 0);
  ui::c::button(s.run_wrap, "リセット",
                [](lv_event_t*) {
                  ui::emit(watch::ActionType::TimerReset);
                },
                nullptr);

  sync_view();
  return scr;
}

void on_event(lv_obj_t*, const watch::Event& e) {
  switch (e.type) {
    case watch::EventType::TimerStarted:
    case watch::EventType::TimerStopped:
    case watch::EventType::TimerFinished:
    case watch::EventType::TimerPaused:
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
