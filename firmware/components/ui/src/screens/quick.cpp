// quick.cpp — クイック設定: 明るさスライダー + よく使う画面への行リンク。
#include "../components.hpp"
#include "../theme.hpp"
#include "screens.hpp"
#include "ui/face_data.hpp"
#include "ui/ui.hpp"

#include <cstdio>

namespace {

lv_obj_t* s_slider_row = nullptr;
bool s_updating = false;

void nav_to(watch::Route r) {
  ui::emit(watch::ActionType::Navigate, static_cast<uint32_t>(r));
}

lv_obj_t* build(lv_obj_t* scr) {
  const ui::Theme& t = ui::theme();
  lv_obj_set_style_bg_color(scr, t.bg, 0);
  ui::c::header(scr, "クイック設定", true);
  lv_obj_t* col = ui::c::content(scr);

  const int32_t bri =
      static_cast<int32_t>(ui::ctx().settings ? ui::ctx().settings->brightness
                                              : 50);
  s_slider_row = ui::c::slider_row(
      col, "明るさ", "%", 5, 100, bri,
      [](lv_event_t* e) {
        if (s_updating) return;
        lv_obj_t* s = lv_event_get_target_obj(e);
        ui::emit(watch::ActionType::SetBrightness,
                 static_cast<uint32_t>(lv_slider_get_value(s)));
      },
      nullptr);

  lv_obj_t* g = ui::c::group(col);

  // 次のアラーム・通知数は補助データから (未配線なら sub なし)。
  const ui::face_data::Snapshot snap = ui::face_data::get();
  char alarm_sub[24] = "";
  if (snap.next_alarm_min >= 0) {
    std::snprintf(alarm_sub, sizeof(alarm_sub), "次は %d:%02d",
                  static_cast<int>(snap.next_alarm_min / 60),
                  static_cast<int>(snap.next_alarm_min % 60));
  }
  char notify_sub[16] = "";
  if (snap.notifications > 0) {
    std::snprintf(notify_sub, sizeof(notify_sub), "%d件",
                  static_cast<int>(snap.notifications));
  }

  ui::c::row_icon(g, "消", lv_color_hex(0x6B7280), "画面を消す", nullptr,
                  true,
                  [](lv_event_t*) { ui::emit(watch::ActionType::ScreenOff); },
                  nullptr);
#if SAS_APP_ALARM
  ui::c::row_icon(g, "ア", lv_color_hex(0xFF8A3D), "アラーム",
                  alarm_sub[0] ? alarm_sub : nullptr, true,
                  [](lv_event_t*) { nav_to(watch::Route::Alarm); }, nullptr);
#endif
#if SAS_APP_NOTIFY
  ui::c::row_icon(g, "通", lv_color_hex(0x4CC9F0), "通知",
                  notify_sub[0] ? notify_sub : nullptr, true,
                  [](lv_event_t*) { nav_to(watch::Route::Notifications); },
                  nullptr);
#endif
#if SAS_APP_MEDIA
  ui::c::row_icon(g, "音", lv_color_hex(0xB388FF), "音楽", nullptr, true,
                  [](lv_event_t*) { nav_to(watch::Route::Media); }, nullptr);
#endif
  ui::c::row_icon(g, "全", lv_color_hex(0x64D6C2), "アプリ一覧", nullptr,
                  true, [](lv_event_t*) { nav_to(watch::Route::More); },
                  nullptr);
  ui::c::row_icon(g, "設", lv_color_hex(0x8A8F9C), "設定", nullptr, true,
                  [](lv_event_t*) { nav_to(watch::Route::Settings); },
                  nullptr);
  return scr;
}

void on_event(lv_obj_t*, const watch::Event& e) {
  if (e.type == watch::EventType::BrightnessChanged && s_slider_row) {
    s_updating = true;
    ui::c::slider_set(s_slider_row, static_cast<int32_t>(e.arg0));
    s_updating = false;
  }
}

}  // namespace

namespace ui {
extern const ScreenOps kQuickScreen = {watch::Route::Quick, build, on_event};
}
