// applist.cpp — アプリ一覧 (More)。色タイル + 行をグループに並べる。
//   テーマ "icons" でアプリ id のタイル画像を差し替えられる
//   (docs/theme-format.md)。id は画面名と揃える。
#include "../components.hpp"
#include "../faces/faces.hpp"
#include "../theme.hpp"
#include "screens.hpp"
#include "ui/face_data.hpp"
#include "ui/ui.hpp"

#include <cstdio>

namespace {

void app_row(lv_obj_t* grp, const char* app_id, const char* icon,
             uint32_t icon_bg, const char* name, const char* sub,
             watch::Route r) {
  // テーマのアイコン画像があればタイルの代わりに使う (無ければ文字タイル)。
  ui::c::row_icon_img(
      grp, ui::theme_icon(app_id), icon, lv_color_hex(icon_bg), name, sub,
      true,
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
  lv_obj_t* g = ui::c::group(col);

  // 今日の歩数は補助データから (未配線なら sub なし)。
  char steps_sub[24] = "";
  const ui::face_data::Snapshot snap = ui::face_data::get();
  if (snap.steps >= 0) {
    char nb[16];
    ui::face::fmt_steps(nb, sizeof(nb), snap.steps);
    std::snprintf(steps_sub, sizeof(steps_sub), "%s 歩", nb);
  }

  app_row(g, "timer", "タ", 0xFF8A3D, "タイマー", nullptr,
          watch::Route::Timer);
  app_row(g, "stopwatch", "ス", 0xFFD166, "ストップウォッチ", nullptr,
          watch::Route::Stopwatch);
  app_row(g, "counter", "カ", 0xC0CA33, "カウンター", nullptr,
          watch::Route::Counter);
  app_row(g, "memo", "メ", 0x64D6C2, "メモ", nullptr, watch::Route::Memo);
  app_row(g, "alarm", "ア", 0xFF7AA2, "アラーム", nullptr,
          watch::Route::Alarm);
  app_row(g, "notifications", "通", 0x4CC9F0, "通知", nullptr,
          watch::Route::Notifications);
  app_row(g, "media", "音", 0xB388FF, "音楽", nullptr, watch::Route::Media);
  app_row(g, "steps", "歩", 0x3DDC97, "歩数", steps_sub,
          watch::Route::Steps);
  app_row(g, "agent", "AI", 0xB388FF, "AI", nullptr, watch::Route::Agent);
  app_row(g, "settings", "設", 0x8A8F9C, "設定", nullptr,
          watch::Route::Settings);
  return scr;
}

}  // namespace

namespace ui {
extern const ScreenOps kMoreScreen = {watch::Route::More, build, nullptr};
}
