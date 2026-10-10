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
extern const ScreenOps kAlarmScreen;
extern const ScreenOps kNotificationsScreen;
extern const ScreenOps kMediaScreen;
extern const ScreenOps kAgentScreen;

const ScreenOps* screen_ops(watch::Route r) {
  switch (r) {
    case watch::Route::Home: return &kHomeScreen;
    case watch::Route::Quick: return &kQuickScreen;
    case watch::Route::More: return &kMoreScreen;
#if SAS_APP_TIMER
    case watch::Route::Timer: return &kTimerScreen;
#endif
#if SAS_APP_STOPWATCH
    case watch::Route::Stopwatch: return &kStopwatchScreen;
#endif
#if SAS_APP_COUNTER
    case watch::Route::Counter: return &kCounterScreen;
#endif
#if SAS_APP_STEPS
    case watch::Route::Steps: return &kStepsScreen;
#endif
#if SAS_APP_MEMO
    case watch::Route::Memo: return &kMemoScreen;
#endif
    case watch::Route::Settings: return &kSettingsScreen;
    case watch::Route::Ota: return &kOtaScreen;
    case watch::Route::PowerMenu: return &kPowerMenuScreen;
#if SAS_APP_ALARM
    case watch::Route::Alarm: return &kAlarmScreen;
#endif
#if SAS_APP_NOTIFY
    case watch::Route::Notifications: return &kNotificationsScreen;
#endif
#if SAS_APP_MEDIA
    case watch::Route::Media: return &kMediaScreen;
#endif
#if SAS_APP_AGENT
    case watch::Route::Agent: return &kAgentScreen;
#endif
    default: return nullptr;
  }
}

}  // namespace ui
