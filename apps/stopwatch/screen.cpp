// stopwatch.cpp — ストップウォッチ: 開始/停止、ラップ(最大20)、リセット。
#include "components.hpp"
#include "faces/faces.hpp"
#include "theme.hpp"

#include <cstdio>
#include "screens/screens.hpp"
#include "ui/port.hpp"
#include "ui/ui.hpp"
#include "watch/features/stopwatch.hpp"

namespace {

struct S {
  lv_obj_t* disp = nullptr;
  lv_obj_t* toggle = nullptr;
  lv_obj_t* toggle_l = nullptr;
  lv_obj_t* laps_col = nullptr;
  lv_obj_t* scr = nullptr;
  lv_timer_t* tick = nullptr;
};
S s;

// timer.cpp と同じく、実行中の離脱で残った lv_timer が解放済み
// ラベルへ書くのを防ぐ。tick は scr の user_data に持たせる。
void kill_tick(lv_obj_t* scr) {
  lv_timer_t* tk = static_cast<lv_timer_t*>(lv_obj_get_user_data(scr));
  if (tk) lv_timer_delete(tk);
  if (s.tick == tk) s.tick = nullptr;
}

void fmt(char* buf, size_t cap, int64_t ms) {
  const int total_cs = static_cast<int>(ms / 10);
  std::snprintf(buf, cap, "%02d:%02d.%02d", total_cs / 6000,
                (total_cs / 100) % 60, total_cs % 100);
}
// ※ 表示フォントは時計フォントなので "." は gen_fonts.py の
//   FACE_FONT_SYMBOLS に含めてある。

void tick_cb(lv_timer_t*) {
  char buf[16];
  fmt(buf, sizeof(buf),
      watch::features::stopwatch_elapsed_ms(ui::port::now_ms()));
  lv_label_set_text(s.disp, buf);
}

void rebuild_laps(lv_obj_t* col) {
  if (!s.laps_col) return;
  lv_obj_clean(s.laps_col);
  const watch::features::StopwatchState& st =
      watch::features::stopwatch_state();
  char buf[48];
  char name[24];
  for (size_t i = 0; i < st.lap_count; ++i) {
    fmt(buf, sizeof(buf), st.laps[i]);
    std::snprintf(name, sizeof(name), "ラップ %u", (unsigned)(i + 1));
    ui::c::row(s.laps_col, name, nullptr, buf, false, nullptr, nullptr);
  }
}

void sync(lv_obj_t* col) {
  const watch::features::StopwatchState& st =
      watch::features::stopwatch_state();
  lv_label_set_text(s.toggle_l, st.running ? "停止" : "開始");
  if (st.running) {
    if (!s.tick) {
      s.tick = lv_timer_create(tick_cb, 50, nullptr);
      lv_obj_set_user_data(s.scr, s.tick);
    }
  } else if (s.tick) {
    lv_timer_delete(s.tick);
    s.tick = nullptr;
    lv_obj_set_user_data(s.scr, nullptr);
  }
  tick_cb(nullptr);
  rebuild_laps(col);
}

lv_obj_t* build(lv_obj_t* scr) {
  const ui::Theme& t = ui::theme();
  lv_obj_set_style_bg_color(scr, t.bg, 0);
  ui::c::header(scr, "ストップウォッチ", true);
  lv_obj_t* col = ui::c::content(scr);
  s = S{};
  s.scr = scr;
  lv_obj_add_event_cb(
      scr, [](lv_event_t* e) { kill_tick(lv_event_get_target_obj(e)); },
      LV_EVENT_DELETE, nullptr);

  // 大きな数字は時計フォント (clock_font 設定)。
  s.disp = lv_label_create(col);
  lv_obj_add_flag(s.disp, LV_OBJ_FLAG_EVENT_BUBBLE);
  // 時計フォント (clock_font)。「00:00.78」は 34px 級が横幅に収まる。
  lv_obj_set_style_text_font(s.disp, ui::face::digits(34), 0);
  lv_obj_set_style_text_color(s.disp, t.text, 0);
  lv_obj_set_style_pad_top(s.disp, 8, 0);

  s.toggle = ui::c::button_primary(col, "開始",
                                 [](lv_event_t*) {
                                   ui::emit(watch::ActionType::StopwatchToggle);
                                 },
                                 nullptr);
  s.toggle_l = lv_obj_get_child(s.toggle, 0);
  ui::c::button(col, "ラップ",
                [](lv_event_t*) { ui::emit(watch::ActionType::StopwatchLap); },
                nullptr);
  ui::c::button(col, "リセット",
                [](lv_event_t*) {
                  ui::emit(watch::ActionType::StopwatchReset);
                },
                nullptr);

  s.laps_col = ui::c::group(col);
  sync(col);
  return scr;
}

void on_event(lv_obj_t* root, const watch::Event& e) {
  if (e.type == watch::EventType::StopwatchChanged) {
    // ラップ数の変更・開始停止・リセットを全部まとめて反映。
    sync(root);
  }
}

}  // namespace

namespace ui {
extern const ScreenOps kStopwatchScreen = {watch::Route::Stopwatch, build, on_event};
}
