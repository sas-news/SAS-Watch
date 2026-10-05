// registry.cpp — Route → ScreenOps の対応表。
#include "screens.hpp"

namespace ui {

extern const ScreenOps kHomeScreen;
extern const ScreenOps kQuickScreen;
extern const ScreenOps kMoreScreen;
extern const ScreenOps kTimerScreen;
extern const ScreenOps kStopwatchScreen;
extern const ScreenOps kCounterScreen;
extern const ScreenOps kMemoScreen;
extern const ScreenOps kSettingsScreen;
extern const ScreenOps kOtaScreen;
extern const ScreenOps kPowerMenuScreen;
extern const ScreenOps kAlarmScreen;
extern const ScreenOps kNotificationsScreen;
extern const ScreenOps kMediaScreen;

const ScreenOps* screen_ops(watch::Route r) {
  switch (r) {
    case watch::Route::Home: return &kHomeScreen;
    case watch::Route::Quick: return &kQuickScreen;
    case watch::Route::More: return &kMoreScreen;
    case watch::Route::Timer: return &kTimerScreen;
    case watch::Route::Stopwatch: return &kStopwatchScreen;
    case watch::Route::Counter: return &kCounterScreen;
    case watch::Route::Memo: return &kMemoScreen;
    case watch::Route::Settings: return &kSettingsScreen;
    case watch::Route::Ota: return &kOtaScreen;
    case watch::Route::PowerMenu: return &kPowerMenuScreen;
    case watch::Route::Alarm: return &kAlarmScreen;
    case watch::Route::Notifications: return &kNotificationsScreen;
    case watch::Route::Media: return &kMediaScreen;
    default: return nullptr;
  }
}

}  // namespace ui
