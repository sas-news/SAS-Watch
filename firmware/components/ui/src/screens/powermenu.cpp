// powermenu.cpp — 電源メニュー (PWR長押し/画面長押しで開く)。
// スリープ / 再起動 / 電源OFFの案内。
#include "../components.hpp"
#include "../theme.hpp"
#include "screens.hpp"
#include "ui/port.hpp"
#include "ui/ui.hpp"

#include <cstdio>

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
  // 注意文は左揃え。font_jp_20 の行高 38 が空きすぎるので line_space で詰める。
  lv_obj_set_flex_align(info, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_t* lt = lv_label_create(info);
  lv_obj_add_flag(lt, LV_OBJ_FLAG_EVENT_BUBBLE);
  // 秒数は AXP2101 の設定値と同じ定数 (board::kPowerOffHoldSeconds) から出す。
  lv_label_set_text_fmt(
      lt, "電源を切るには:\nPWRボタンを%d秒以上\n長押しすると強制OFFです",
      ui::port::power_off_hold_seconds());
  lv_obj_set_style_text_font(lt, t.font_body, 0);
  lv_obj_set_style_text_color(lt, t.text_dim, 0);
  lv_obj_set_style_text_line_space(lt, -12, 0);
  lv_obj_set_width(lt, LV_PCT(100));
  // TODO(hw): 実機で確認 — PMU 長押し強制OFFの可否 (AXP2101 PWROK 設定)
  return scr;
}

}  // namespace

namespace ui {
extern const ScreenOps kPowerMenuScreen = {watch::Route::PowerMenu, build, nullptr};
}
