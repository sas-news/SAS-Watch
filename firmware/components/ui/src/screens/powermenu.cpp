// powermenu.cpp — 電源メニュー (PWR長押し/画面長押しで開く)。
// スリープ / 再起動 / 電源OFFの案内。
#include "../components.hpp"
#include "../theme.hpp"
#include "screens.hpp"
#include "ui/port.hpp"
#include "ui/ui.hpp"

namespace {

lv_obj_t* build(lv_obj_t* scr) {
  const ui::Theme& t = ui::theme();
  lv_obj_set_style_bg_color(scr, t.bg, 0);
  ui::c::header(scr, "電源メニュー", true);
  lv_obj_t* col = ui::c::content(scr);

  ui::c::button(col, "スリープ (画面OFF)",
                [](lv_event_t*) {
                  ui::emit(watch::ActionType::Home);
                  ui::emit(watch::ActionType::ScreenOff);
                },
                nullptr);
  ui::c::button(col, "再起動",
                [](lv_event_t*) { ui::port::restart(); }, nullptr);

  lv_obj_t* info = ui::c::card(col);
  ui::c::line(info, "電源を切るには:");
  ui::c::line(info, "PWRボタンを10秒以上");
  ui::c::line(info, "長押しすると強制OFFです");
  // TODO(hw): 実機で確認 — PMU 長押し強制OFFの可否 (AXP2101 PWROK 設定)
  return scr;
}

}  // namespace

namespace ui {
extern const ScreenOps kPowerMenuScreen = {watch::Route::PowerMenu, build, nullptr};
}
