// registry.cpp — Route → ScreenOps の対応表。
// 本体側の画面はこの switch に残し、アプリ (apps/*/app.cmake) の画面は
// 生成コード app_ui.cpp の app_screen_ops() に委譲する (T19)。
#include "screens.hpp"

#include "app_ui.hpp"

namespace ui {

extern const ScreenOps kHomeScreen;
extern const ScreenOps kQuickScreen;
extern const ScreenOps kMoreScreen;
extern const ScreenOps kSettingsScreen;
extern const ScreenOps kOtaScreen;
extern const ScreenOps kPowerMenuScreen;

const ScreenOps* screen_ops(watch::Route r) {
  switch (r) {
    case watch::Route::Home: return &kHomeScreen;
    case watch::Route::Quick: return &kQuickScreen;
    case watch::Route::More: return &kMoreScreen;
    case watch::Route::Settings: return &kSettingsScreen;
    case watch::Route::Ota: return &kOtaScreen;
    case watch::Route::PowerMenu: return &kPowerMenuScreen;
    default: break;
  }
  return app_screen_ops(r);
}

}  // namespace ui
