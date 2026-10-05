// applist.cpp — アプリ一覧 (More)。縦に並べるだけのシンプルなメニュー。
#include "../components.hpp"
#include "../theme.hpp"
#include "screens.hpp"
#include "ui/ui.hpp"

namespace {

void row(lv_obj_t* col, const char* name, watch::Route r) {
  ui::c::list_row(col, name, nullptr,
                  [](lv_event_t* e) {
                    const auto r = static_cast<watch::Route>(
                        reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
                    ui::emit(watch::ActionType::Navigate,
                             static_cast<uint32_t>(r));
                  },
                  reinterpret_cast<void*>(static_cast<uintptr_t>(r)));
}

lv_obj_t* build(lv_obj_t* scr) {
  const ui::Theme& t = ui::theme();
  lv_obj_set_style_bg_color(scr, t.bg, 0);
  ui::c::header(scr, "アプリ", true);
  lv_obj_t* col = ui::c::content(scr);
  row(col, "タイマー", watch::Route::Timer);
  row(col, "ストップウォッチ", watch::Route::Stopwatch);
  row(col, "カウンター", watch::Route::Counter);
  row(col, "メモ", watch::Route::Memo);
  row(col, "AI", watch::Route::Agent);
  row(col, "設定", watch::Route::Settings);
  return scr;
}

}  // namespace

namespace ui {
extern const ScreenOps kMoreScreen = {watch::Route::More, build, nullptr};
}
