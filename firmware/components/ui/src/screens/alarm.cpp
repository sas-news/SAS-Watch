// alarm.cpp — アラーム: 一覧 + ON/OFF トグル。
// 追加・編集はスマホアプリ (alarm.list/alarm.set/alarm.delete) から。
// 行タップで有効/無効を切替え、鳴動中は shell のアラートが前面に出る。
#include "../components.hpp"
#include "../theme.hpp"
#include "screens.hpp"
#include "ui/ui.hpp"
#include "watch/features/alarm.hpp"

#include <cstdio>

namespace {

lv_obj_t* s_list_box = nullptr;

// dow マスク → "毎日" / "日月火..." の短い表記。
void fmt_dow(uint8_t dow, char* buf, size_t cap) {
  if (dow == 0) {
    std::snprintf(buf, cap, "毎日");
    return;
  }
  static const char* names[7] = {"日", "月", "火", "水", "木", "金", "土"};
  size_t n = 0;
  for (int i = 0; i < 7 && n + 4 < cap; ++i) {
    if (dow & (1u << i)) {
      n += static_cast<size_t>(
          std::snprintf(buf + n, cap - n, "%s", names[i]));
    }
  }
  buf[n] = '\0';
}

void fit_in_card(lv_obj_t* o) { lv_obj_set_width(o, LV_PCT(100)); }

void rebuild_list() {
  lv_obj_clean(s_list_box);
  const size_t n = watch::features::alarm_count();
  if (n == 0) {
    ui::c::line(s_list_box, "アラームはまだありません");
    ui::c::line(s_list_box, "スマホアプリから追加できます");
    return;
  }
  for (size_t i = 0; i < n; ++i) {
    watch::features::AlarmEntry a;
    if (!watch::features::alarm_at(i, &a)) continue;
    char time_s[8];
    std::snprintf(time_s, sizeof(time_s), "%u:%02u", a.hour, a.min);
    char dow_s[32];
    fmt_dow(a.dow, dow_s, sizeof(dow_s));
    char sub[48];
    std::snprintf(sub, sizeof(sub), "%s %s", dow_s,
                  a.enabled ? "ON" : "OFF");
    lv_obj_t* row = ui::c::list_row(
        s_list_box, time_s, sub,
        [](lv_event_t* e) {
          const uint32_t id = static_cast<uint32_t>(
              reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
          ui::emit(watch::ActionType::AlarmToggle, id);
        },
        reinterpret_cast<void*>(static_cast<uintptr_t>(a.id)));
    fit_in_card(row);
    if (lv_obj_t* l = lv_obj_get_child(row, 0)) {
      lv_obj_set_width(l, LV_PCT(100));
    }
  }
}

lv_obj_t* build(lv_obj_t* scr) {
  const ui::Theme& t = ui::theme();
  lv_obj_set_style_bg_color(scr, t.bg, 0);
  ui::c::header(scr, "アラーム", true);
  lv_obj_t* col = ui::c::content(scr);

  lv_obj_t* hint = ui::c::card(col);
  ui::c::line(hint, "追加・編集はスマホアプリから");

  s_list_box = ui::c::card(col);
  rebuild_list();
  return scr;
}

void on_event(lv_obj_t*, const watch::Event& e) {
  if (e.type == watch::EventType::AlarmChanged && s_list_box) rebuild_list();
}

}  // namespace

namespace ui {
extern const ScreenOps kAlarmScreen = {watch::Route::Alarm, build, on_event};
}
