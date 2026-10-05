// registry.cpp — Route → ScreenOps の対応表。
#include "screens.hpp"

namespace ui {

extern const ScreenOps kHomeScreen;
extern const ScreenOps kQuickScreen;
extern const ScreenOps kMoreScreen;
extern const ScreenOps kTimerScreen;
extern const ScreenOps kStopwatchScreen;
extern const ScreenOps kCounterScreen;
extern const ScreenOps kStepsScreen;
extern const ScreenOps kMemoScreen;
extern const ScreenOps kSettingsScreen;
extern const ScreenOps kOtaScreen;
extern const ScreenOps kPowerMenuScreen;
extern const ScreenOps kAgentScreen;

const ScreenOps* screen_ops(watch::Route r) {
  switch (r) {
    case watch::Route::Home: return &kHomeScreen;
    case watch::Route::Quick: return &kQuickScreen;
    case watch::Route::More: return &kMoreScreen;
    case watch::Route::Timer: return &kTimerScreen;
    case watch::Route::Stopwatch: return &kStopwatchScreen;
    case watch::Route::Counter: return &kCounterScreen;
    case watch::Route::Steps: return &kStepsScreen;
    case watch::Route::Memo: return &kMemoScreen;
    case watch::Route::Settings: return &kSettingsScreen;
    case watch::Route::Ota: return &kOtaScreen;
    case watch::Route::PowerMenu: return &kPowerMenuScreen;
    case watch::Route::Agent: return &kAgentScreen;
    default: return nullptr;
  }
}

}  // namespace ui
