// counter.cpp — カウンター: ＋/−/リセット。
#include "../components.hpp"
#include "../theme.hpp"

#include <cstdio>
#include "screens.hpp"
#include "ui/ui.hpp"
#include "watch/features/counter.hpp"

namespace {

lv_obj_t* s_num = nullptr;

void refresh() {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%d",
                static_cast<int>(watch::features::counter_value()));
  lv_label_set_text(s_num, buf);
}

lv_obj_t* build(lv_obj_t* scr) {
  const ui::Theme& t = ui::theme();
  lv_obj_set_style_bg_color(scr, t.bg, 0);
  ui::c::header(scr, "カウンター", true);
  lv_obj_t* col = ui::c::content(scr);

  lv_obj_t* card = ui::c::card(col);
  lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  s_num = lv_label_create(card);
  lv_obj_set_style_text_font(s_num, t.font_digits_sm, 0);
  lv_obj_set_style_text_color(s_num, t.accent, 0);

  ui::c::button_primary(
      col, "＋1",
      [](lv_event_t*) { ui::emit(watch::ActionType::CounterAdd, 1); },
      nullptr);
  ui::c::button(col, "−1",
                [](lv_event_t*) {
                  ui::emit(watch::ActionType::CounterAdd,
                           static_cast<uint32_t>(-1));
                },
                nullptr);
  ui::c::button(col, "リセット",
                [](lv_event_t*) { ui::emit(watch::ActionType::CounterReset); },
                nullptr);
  refresh();
  return scr;
}

void on_event(lv_obj_t*, const watch::Event& e) {
  if (e.type == watch::EventType::CounterChanged) refresh();
}

}  // namespace

namespace ui {
extern const ScreenOps kCounterScreen = {watch::Route::Counter, build, on_event};
}
