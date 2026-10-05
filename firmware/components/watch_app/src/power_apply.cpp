// power_apply.cpp — 電源状態 → ハード反映。
//   Active    : 輝度=ユーザー設定、パネルON、LVGLタイマー再開
//   Dim       : 輝度=20% (plan.md L章)
//   ScreenOff : 輝度0 + esp_lcd_panel_disp_on_off(false) + lv_timer 停止
// Deep Sleep への移行は watch_app.cpp のタスクが行う (保存・起床設定があるため)。
#include "power_apply.hpp"

#include "esp_log.h"
#include "lvgl.h"
#include "ui/port.hpp"

namespace watch_app {

static const char* TAG = "watch_app.power";
static watch::PowerState s_applied = watch::PowerState::Active;
static constexpr int kDimPercent = 20;  // plan.md L章: Dim = 輝度20%

void power_apply(watch::PowerState st, const watch::Settings& settings) {
  if (st == s_applied) return;
  s_applied = st;
  ESP_LOGI(TAG, "power -> %d", static_cast<int>(st));

  // LVGL を触るのは LVGL タスク内かロックの中だけ (AGENTS.md)。
  if (!ui::port::lock(200)) {
    ESP_LOGW(TAG, "lvgl lock timeout; apply deferred");
    s_applied = watch::PowerState::Active;  // 次の変化で再適用させる
    return;
  }
  switch (st) {
    case watch::PowerState::Active:
      lv_timer_enable(true);
      ui::port::display_power(true);
      ui::port::brightness_apply(static_cast<int>(settings.brightness));
      break;
    case watch::PowerState::Dim:
      lv_timer_enable(true);
      ui::port::display_power(true);
      ui::port::brightness_apply(kDimPercent);
      break;
    case watch::PowerState::ScreenOff:
    case watch::PowerState::DeepSleepCandidate:
      lv_timer_enable(false);  // 画面OFF中は描画しない
      ui::port::brightness_apply(0);
      ui::port::display_power(false);
      break;
  }
  ui::port::unlock();
}

}  // namespace watch_app
