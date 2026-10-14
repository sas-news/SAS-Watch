// GENERATED — apps/*/app.cmake から自動生成。編集しない。
#include "/home/sasnews/Desktop/ai-space/SAS-Watch/firmware/components/ui/src/screens/screens.hpp"
#include "app_ui.hpp"

namespace ui { extern const ScreenOps kAgentScreen; }
namespace ui { extern const ScreenOps kAlarmScreen; }
namespace ui { extern const ScreenOps kCounterScreen; }
namespace ui { extern const ScreenOps kMediaScreen; }
namespace ui { extern const ScreenOps kMemoScreen; }
namespace ui { extern const ScreenOps kNotificationsScreen; }
namespace ui { extern const ScreenOps kStepsScreen; }
namespace ui { const char* steps_applist_sub(); }
namespace ui { extern const ScreenOps kStopwatchScreen; }
namespace ui { extern const ScreenOps kTimerScreen; }
namespace ui { extern const ScreenOps kTemplateAppScreen; }

namespace ui {
const ScreenOps* app_screen_ops(watch::Route r) {
  switch (r) {
    case watch::Route::Agent: return &kAgentScreen;
    case watch::Route::Alarm: return &kAlarmScreen;
    case watch::Route::Counter: return &kCounterScreen;
    case watch::Route::Media: return &kMediaScreen;
    case watch::Route::Memo: return &kMemoScreen;
    case watch::Route::Notifications: return &kNotificationsScreen;
    case watch::Route::Steps: return &kStepsScreen;
    case watch::Route::Stopwatch: return &kStopwatchScreen;
    case watch::Route::Timer: return &kTimerScreen;
    case watch::Route::App0: return &kTemplateAppScreen;
    default: return nullptr;
  }
}
const SasAppRow kSasAppRows[] = {
  {"timer", u8"\uE425", 0xFF8A3D, "タイマー", nullptr, watch::Route::Timer},
  {"stopwatch", u8"\uF4BA", 0xFFD166, "ストップウォッチ", nullptr, watch::Route::Stopwatch},
  {"counter", u8"\uE800", 0xC0CA33, "カウンター", nullptr, watch::Route::Counter},
  {"memo", u8"\uE31D", 0x64D6C2, "メモ", nullptr, watch::Route::Memo},
  {"alarm", u8"\uE855", 0xFF7AA2, "アラーム", nullptr, watch::Route::Alarm},
  {"template", u8"\uE800", 0x8A8F9C, "サンプル", nullptr, watch::Route::App0},
  {"notify", u8"\uE7F5", 0x4CC9F0, "通知", nullptr, watch::Route::Notifications},
  {"media", u8"\uE405", 0xB388FF, "音楽", nullptr, watch::Route::Media},
  {"steps", u8"\uE536", 0x3DDC97, "歩数", steps_applist_sub, watch::Route::Steps},
  {"agent", u8"\uF06C", 0xB388FF, "AI", nullptr, watch::Route::Agent},
};
}  // namespace ui
