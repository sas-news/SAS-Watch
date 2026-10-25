// power_apply.cpp — 電源状態 → ハード反映。
//   Active    : 輝度=ユーザー設定、パネルON、LVGLタイマー再開
//   Dim       : 輝度=20% (plan.md L章)
//   ScreenOff : 輝度0 + esp_lcd_panel_disp_on_off(false) + lv_timer 停止
// Deep Sleep への移行は watch_app.cpp のタスクが行う (保存・起床設定があるため)。
#include "power_apply.hpp"

#include "ble_link.h"
#include "esp_log.h"
#include "lvgl.h"
#include "ui/port.hpp"

namespace watch_app {

static const char* TAG = "watch_app.power";
static watch::PowerState s_applied = watch::PowerState::Active;
static constexpr int kDimPercent = 20;  // plan.md L章: Dim = 輝度20%

void power_apply(watch::PowerState st, const watch::Settings& settings) {
  if (st == s_applied) return;

  // LVGL を触るのは LVGL タスク内かロックの中だけ (AGENTS.md)。
  // ロックが取れなかったら s_applied を進めずに次ループで再試行する。
  // 以前は s_applied を Active にして諦めていたので、点灯の適用が
  // 一度でも失敗すると「状態は Active なのにパネルは消えたまま」になり、
  // PWR を押しても消灯要求だけが走って画面が点かなくなっていた。
  if (!ui::port::lock(200)) {
    static uint32_t s_lock_fail;
    if ((s_lock_fail++ % 50) == 0) {
      ESP_LOGW(TAG, "lvgl lock timeout; retry (%lu)",
               static_cast<unsigned long>(s_lock_fail));
    }
    return;
  }
  ESP_LOGI(TAG, "power %d -> %d", static_cast<int>(s_applied),
           static_cast<int>(st));
  s_applied = st;
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

  // 画面OFF中ポリシー (C案改): 広告は止めず低頻度化して再接続窓を残す
  // (停止にすると一度切れた電話が再接続できず通知が届かない)。
  // 既存の接続は維持。画面ONに戻ると通常間隔に戻す。
  ble_link_set_advertising(true);
  ble_link_set_adv_rate(st == watch::PowerState::ScreenOff ||
                        st == watch::PowerState::DeepSleepCandidate);
}

}  // namespace watch_app
