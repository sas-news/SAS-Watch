// screen.cpp — 雛形アプリの画面。ui::c 部品だけで組む。
// 画面は Route に紐づく ScreenOps として登録される
// (app.cmake の APP_SCREEN_SYM がこのシンボルを指す)。
#include "components.hpp"
#include "screens/screens.hpp"
#include "ui/ui.hpp"

#include <cstdio>

#include "feature/template_app.hpp"

namespace {

lv_obj_t* s_num = nullptr;

lv_obj_t* build(lv_obj_t* scr) {
  const ui::Theme& t = ui::theme();
  lv_obj_set_style_bg_color(scr, t.bg, 0);
  ui::c::header(scr, "サンプル", true);
  lv_obj_t* col = ui::c::content(scr);
  lv_obj_t* card = ui::c::card(col);
  s_num = ui::c::line(card, "0");
  char b[16];
  std::snprintf(b, sizeof(b), "%ld",
                static_cast<long>(watch::features::template_app_count()));
  lv_label_set_text(s_num, b);
  ui::c::button_primary(card, "＋1",
    [](lv_event_t*) {
      ui::emit(watch::ActionType::PrimaryAction);
    }, nullptr);
  return scr;
}

void on_event(lv_obj_t*, const watch::Event& e) {
  if (e.type == watch::EventType::SettingsChanged && s_num) {
    char b[16];
    std::snprintf(b, sizeof(b), "%ld",
                  static_cast<long>(watch::features::template_app_count()));
    lv_label_set_text(s_num, b);
  }
}

}  // namespace

namespace ui {
extern const ScreenOps kTemplateAppScreen = {
    watch::Route::App0, build, on_event};
}
