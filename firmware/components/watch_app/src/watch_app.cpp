// watch_app.cpp — app タスク本体。
//   キューを1件処理 → tick → power_apply → 眠る (notify か deadline で起床)。
//   DeepSleepCandidate で queue 空なら保存して board::enter_deep_sleep。
#include "watch_app/watch_app.hpp"

#include "internal.hpp"

#include "audio/audio.hpp"
#include "board/board.hpp"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "power_apply.hpp"
#include "ui/port.hpp"
#include "ui/ui.hpp"
#include "watch/input_mapper.hpp"
#include "watch/platform.hpp"

namespace watch_app {

void input_init(watch::InputMapper* mapper);  // input.cpp

namespace {

constexpr const char* TAG = "watch_app";

// 全て静的確保 (起動時のみの確保ルール)。
watch::EventBus s_bus;
watch::Navigator s_nav(&s_bus);
watch::PowerPolicy s_power(&s_bus);
watch::InputMapper s_input;
watch::FeatureRegistry s_features(watch::builtin_features(),
                                  watch::builtin_features_count());
watch::Settings s_settings;
watch::Runtime s_rt(s_bus, s_nav, s_power, s_input, s_features, s_settings);

// FeatureContext は参照型なので起動時に一度だけ組み立てる。
watch::Clock* s_clock = nullptr;
watch::KeyValueStore* s_kv = nullptr;
watch::FeatureContext* s_fctx = nullptr;

TaskHandle_t s_task = nullptr;
SemaphoreHandle_t s_core_mtx = nullptr;
bool s_started = false;

void ble_cb(const watch::Event& e, void*) {
  // BLE 接続中は Deep Sleep に行かない (plan.md L章)。
  s_power.set_ble_connected(e.arg0 != 0);
}

void enter_deep_sleep() {
  ESP_LOGI(TAG, "deep sleep");
  s_features.save_all(*s_fctx);
  const int64_t dl = s_rt.next_deadline_ms();
  const int64_t now = s_clock->now_ms();
  const uint64_t us = dl > now ? static_cast<uint64_t>(dl - now) * 1000 : 0;
  // 起床: BOOT(ext0) / PWR+IMU(ext1) / 次の期限でタイマー。
  // raise_to_wake が OFF なら WoM はかけない (INT1 は出ないので
  // ext1 側はそのままでよい)。
  if (s_settings.raise_to_wake) {
    board::imu::arm_wake_on_motion();
  }
  board::pmic::panel_power(false);  // ALDO2 OFF // TODO(hw): 復帰時間
  board::sleep::enter_deep_sleep(us);
}

void app_task(void*) {
  for (;;) {
    ble_glue_poll();

    const int64_t now = s_clock->now_ms();
    core_lock();
    s_rt.step(now, *s_fctx);
    core_unlock();
    power_apply(s_power.state(), s_settings);

    if (s_power.wants_deep_sleep() && s_rt.queue().empty()) {
      enter_deep_sleep();  // 戻らない
    }

    // 次の期限まで寝る。キュー投入の notify で即起きる。
    TickType_t wait = portMAX_DELAY;
    const int64_t dl = s_rt.next_deadline_ms();
    if (dl > 0) {
      const int64_t d = dl - s_clock->now_ms();
      wait = d <= 0 ? 1 : pdMS_TO_TICKS(static_cast<uint64_t>(d));
      if (wait == 0) wait = 1;
    }
    ulTaskNotifyTake(pdTRUE, wait);
  }
}

void log_wake_reason() {
  const esp_sleep_wakeup_cause_t c = esp_sleep_get_wakeup_cause();
  const char* s = "reset";
  switch (c) {
    case ESP_SLEEP_WAKEUP_EXT0: s = "ext0 (BOOT)"; break;
    case ESP_SLEEP_WAKEUP_EXT1: s = "ext1 (PWR/IMU)"; break;
    case ESP_SLEEP_WAKEUP_TIMER: s = "timer"; break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD: s = "touchpad"; break;
    case ESP_SLEEP_WAKEUP_ULP: s = "ulp"; break;
    case ESP_SLEEP_WAKEUP_GPIO: s = "gpio"; break;
    case ESP_SLEEP_WAKEUP_UART: s = "uart"; break;
    default: s = "power-on/reset"; break;
  }
  ESP_LOGI(TAG, "wake reason: %s", s);
}

}  // namespace

bool start(const Deps& deps) {
  if (s_started || !deps.clock || !deps.kv) return false;
  s_clock = deps.clock;
  s_kv = deps.kv;
  s_core_mtx = xSemaphoreCreateMutex();

  log_wake_reason();

  // 設定の復元 → 電源しきい値・ボタン割り当てに反映。
  watch::settings_load(s_settings, *s_kv);
  s_power.configure(s_settings.dim_after_s, s_settings.screen_off_after_s,
                    s_settings.deep_sleep_after_s);
  s_input.map(watch::PhysicalButton::Boot, watch::PressType::Short,
              s_settings.button_boot_short);
  s_input.map(watch::PhysicalButton::Boot, watch::PressType::Long,
              s_settings.button_boot_long);
  s_input.map(watch::PhysicalButton::Boot, watch::PressType::Double,
              s_settings.button_boot_double);
  s_input.map(watch::PhysicalButton::Pwr, watch::PressType::Short,
              s_settings.button_pwr_short);
  s_input.map(watch::PhysicalButton::Pwr, watch::PressType::Long,
              s_settings.button_pwr_long);
  s_input.map(watch::PhysicalButton::Pwr, watch::PressType::Double,
              s_settings.button_pwr_double);

  static watch::FeatureContext fctx{s_bus, *s_kv, *s_clock, &s_nav, &s_power};
  s_fctx = &fctx;
  fctx.settings = &s_settings;
  // Phase 9: 音声サービス (littlefs の storage パーティション + コーデック)。
  // 失敗しても audio 無しで動く (録音ボタンは出ない)。
  audio::Deps audio_deps;
  audio_deps.power = &s_power;
  audio_deps.settings = &s_settings;
  if (audio::init(audio_deps)) {
    fctx.audio = audio::port();
    audio::attach(s_bus);
  }
  s_rt.init(fctx);
  s_features.restore_all(fctx);

  s_bus.subscribe(watch::EventType::BleConnChanged, ble_cb, nullptr);
  ble_glue_init();

  // UI の Action 出口をこのキューへ。
  ui::set_action_sink(&push_action);
  input_init(&s_input);

  // 起動時輝度 = 保存値。画面ONは ui::init_display 側。
  ui::port::brightness_apply(static_cast<int>(s_settings.brightness));

  // esp_pm: 動的クロック + 自動 light sleep (sdkconfig PM_ENABLE/TICKLESS)。
  const esp_pm_config_t pm_cfg = {
      .max_freq_mhz = 240,
      .min_freq_mhz = 40,
      .light_sleep_enable = true,  // TODO(hw): 実機で確認 — touch/panel の復帰
  };
  const esp_err_t pm_ret = esp_pm_configure(&pm_cfg);
  if (pm_ret != ESP_OK) {
    ESP_LOGW(TAG, "esp_pm_configure: %s", esp_err_to_name(pm_ret));
  }
  // light sleep からの起床を配線 (タッチ INT/BOOT/PWR)。
  board::sleep::arm_light_sleep_wake();

  const BaseType_t ok =
      xTaskCreate(app_task, "watch_app", 8192, nullptr, 5, &s_task);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "xTaskCreate failed");
    return false;
  }
  // IMU 常時サービス (歩数 + raise-to-wake)。app タスクの後で起こす。
  imu_service_start();
  s_started = true;
  return true;
}

void push_action(const watch::Action& a) {
  if (!s_rt.queue().push(a)) {
    ESP_LOGW(TAG, "action queue full; dropped %d", static_cast<int>(a.type));
    return;
  }
  if (s_task) xTaskNotifyGive(s_task);
}

watch::Runtime& runtime() { return s_rt; }
watch::Navigator& navigator() { return s_nav; }
watch::EventBus& bus() { return s_bus; }
const watch::Settings& settings() { return s_settings; }

// ---- 内部公開口 (internal.hpp) ---------------------------------------------

void wake_task() {
  if (s_task) xTaskNotifyGive(s_task);
}

void core_lock() {
  if (s_core_mtx) xSemaphoreTake(s_core_mtx, portMAX_DELAY);
}
void core_unlock() {
  if (s_core_mtx) xSemaphoreGive(s_core_mtx);
}

watch::PowerPolicy& power() { return s_power; }
watch::InputMapper& input() { return s_input; }
watch::FeatureRegistry& features() { return s_features; }
watch::FeatureContext* fctx() { return s_fctx; }
watch::Clock* clock() { return s_clock; }
watch::KeyValueStore* kv() { return s_kv; }
watch::Settings& settings_mut() { return s_settings; }

}  // namespace watch_app
