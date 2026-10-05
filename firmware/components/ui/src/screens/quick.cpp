// quick.cpp — クイック設定: 明るさ、画面OFF、アプリへのリンク。
#include "../components.hpp"
#include "../theme.hpp"
#include "screens.hpp"
#include "ui/ui.hpp"

namespace {

lv_obj_t* s_slider = nullptr;
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
  s_slider = ui::c::slider_of(ui::c::slider_row(
      col, "明るさ", 5, 100, bri,
      [](lv_event_t* e) {
        if (s_updating) return;
        lv_obj_t* s = lv_event_get_target_obj(e);
        ui::emit(watch::ActionType::SetBrightness,
                 static_cast<uint32_t>(lv_slider_get_value(s)));
      },
      nullptr));

  ui::c::button(col, "画面を消す",
                [](lv_event_t*) { ui::emit(watch::ActionType::ScreenOff); },
                nullptr);
  ui::c::button(col, "アプリ一覧",
                [](lv_event_t*) { nav_to(watch::Route::More); }, nullptr);
  ui::c::button(col, "アラーム",
                [](lv_event_t*) { nav_to(watch::Route::Alarm); }, nullptr);
  ui::c::button(col, "通知",
                [](lv_event_t*) { nav_to(watch::Route::Notifications); },
                nullptr);
  ui::c::button(col, "音楽",
                [](lv_event_t*) { nav_to(watch::Route::Media); }, nullptr);
  ui::c::button(col, "設定",
                [](lv_event_t*) { nav_to(watch::Route::Settings); }, nullptr);
  return scr;
}

void on_event(lv_obj_t*, const watch::Event& e) {
  if (e.type == watch::EventType::BrightnessChanged && s_slider) {
    s_updating = true;
    lv_slider_set_value(s_slider, static_cast<int32_t>(e.arg0), LV_ANIM_OFF);
    s_updating = false;
  }
}

}  // namespace

namespace ui {
extern const ScreenOps kQuickScreen = {watch::Route::Quick, build, on_event};
}
