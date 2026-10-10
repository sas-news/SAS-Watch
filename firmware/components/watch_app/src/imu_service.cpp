// imu_service.cpp — QMI8658 のホスト側サービス (plan.md の sensor タスク)。
//   画面ON (Active/Dim): 低頻度ポーリング → ImuSample Action (腕上げ判定)
//   画面OFF: WoM + GPIO21 で light sleep 起床 → 短いバーストを同じく
//            ImuSample へ (core の RaiseDetector が判定 → RaiseDetected
//            → ここが Wake Action に変換)。Deep Sleep 中の起床は
//            ext1(GPIO21) + IMU WoM で戻る (watch_app.cpp 側)。
// 歩数はチップ内蔵 pedometer (hw_pedometer_init 成功時) が主経路:
//   画面OFF中もチップが数え続けるので、こちらは起床時・5分RTC周期・
//   画面ON中の定期タイミングで STEP_CNT を読み StepsHwSync へ流すだけ。
//   init 失敗 (チップ非対応等) ならソフト検出 (core の StepDetector が
//   ImuSample を食う) に自動フォールバック。
#include "internal.hpp"

#include "board/board.hpp"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "watch/action.hpp"
#include "watch/event.hpp"
#include "watch/power.hpp"

namespace watch_app {
namespace {

constexpr const char* TAG = "imu_svc";

// TODO(hw): 各周期は実機で歩数/腕上げの取りこぼしを見て調整。
constexpr uint32_t kPollMs = 50;      // 画面ONの加速度取得間隔 (~20Hz)
constexpr uint32_t kIdleCheckMs = 300;  // 画面OFF時の起床原因チェック間隔
constexpr uint32_t kBurstN = 8;       // GPIO起床直後の連続サンプル数
constexpr uint32_t kBurstMs = 35;     // バースト内の間隔 (hold_ms 200 を満たす)
constexpr uint32_t kHwSyncPollMs = 2000;    // 画面ON中の歩数同期間隔
constexpr uint64_t kHwRtcSyncUs = 5ull * 60 * 1000000;  // 画面OFF中5分周期

TaskHandle_t s_task = nullptr;
bool s_hw_ped = false;          // ハード歩数計が有効か
int64_t s_next_hw_sync_ms = 0;  // 次に STEP_CNT を読む時刻

// queue 経由で app タスクへ ImuSample を投げる。
// source=System: 測定は「操作」ではないので電源タイマーを蹴らない
// (画面OFF中のバーストでも画面を ON にしない。判断は core に任せる)。
void push_sample(int16_t x, int16_t y, int16_t z) {
  watch::Action a{};
  a.type = watch::ActionType::ImuSample;
  a.source = watch::ActionSource::System;
  a.arg0 = static_cast<uint16_t>(x) |
           (static_cast<uint32_t>(static_cast<uint16_t>(y)) << 16);
  a.arg1 = static_cast<uint16_t>(z);
  watch_app::push_action(a);
}

// HW 歩数計の累積カウンタ (24bit) を core へ流す。
// core 側で差分に変換して当日歩数へ加算する。
void push_hw_sync(uint32_t total) {
  watch::Action a{};
  a.type = watch::ActionType::StepsHwSync;
  a.source = watch::ActionSource::System;
  a.arg0 = total;
  watch_app::push_action(a);
}

bool screen_on();

// 同期時刻が来ていれば STEP_CNT を読んで送る。期限はここで管理
// (light sleep またぎでも esp_timer は実時間なので取りこぼさない)。
void maybe_sync_hw() {
  if (!s_hw_ped) return;
  const int64_t now = esp_timer_get_time() / 1000;
  if (now < s_next_hw_sync_ms) return;
  uint32_t cnt = 0;
  if (board::imu::hw_pedometer_steps(&cnt) == ESP_OK) {
    push_hw_sync(cnt);
  } else {
    ESP_LOGW(TAG, "STEP_CNT read failed");
  }
  // 画面ON中は 2s 周期、画面OFF中は 5分周期に再設定する。
  s_next_hw_sync_ms = now + (screen_on() ? kHwSyncPollMs
                                         : kHwRtcSyncUs / 1000);
}

bool read_sample(int16_t* x, int16_t* y, int16_t* z) {
  float fx = 0, fy = 0, fz = 0;
  if (board::imu::read_accel_mg(&fx, &fy, &fz) != ESP_OK) return false;
  *x = static_cast<int16_t>(fx);
  *y = static_cast<int16_t>(fy);
  *z = static_cast<int16_t>(fz);
  return true;
}

bool screen_on() {
  const watch::PowerState s = power().state();
  return s == watch::PowerState::Active || s == watch::PowerState::Dim;
}

// ---- 画面OFF中の起床検出 ---------------------------------------------------
// light sleep では CPU 自体が眠るので、GPIO21(INT1) 起床後に誰も即座には
// 動かない。idle 中は何度も light sleep を繰り返すので、センサータスクが
// 短い周期で起床原因レジスタを読み、「GPIO で起きた」直後だけバーストする。
// 連続同じ原因は 1 度として扱う (次に timer 等で wake した時点で解除される)。

enum class ImuMode : uint8_t { Poll, IdleArmed, IdleDisarmed };
ImuMode s_mode = ImuMode::Poll;

esp_sleep_wakeup_cause_t s_last_cause = ESP_SLEEP_WAKEUP_UNDEFINED;

// 直近の起床原因の「変化」を見る。GPIO 起床であれば IMU/タッチ/ボタン
// 由来の候補としてバーストを取り、向き判定は RaiseDetector に委ねる。
// 起床原因レジスタは GPIO 起床同士を区別できない (連続 GPIO 起床は
// 同じ原因のまま見える) ので、非 GPIO 起床で原因が変わるまでは
// dedup される — 元実装の INT1 ピン HIGH 判定はパルス式 INT だと
// 読取時に LOW へ戻っていて取りこぼすため使わない。
bool gpio_woke() {
  const esp_sleep_wakeup_cause_t c = esp_sleep_get_wakeup_cause();
  if (c == s_last_cause) return false;
  s_last_cause = c;
  return c == ESP_SLEEP_WAKEUP_GPIO;
}

void burst() {
  // 起床直後の向きを core の RaiseDetector に流す。
  int16_t x, y, z;
  for (uint32_t i = 0; i < kBurstN; ++i) {
    if (read_sample(&x, &y, &z)) push_sample(x, y, z);
    if (i + 1 < kBurstN) vTaskDelay(pdMS_TO_TICKS(kBurstMs));
  }
}

void apply_idle_mode() {
  if (settings_mut().raise_to_wake) {
    if (s_mode != ImuMode::IdleArmed) {
      board::imu::arm_wake_on_motion();
      board::sleep::arm_light_sleep_imu();
      s_mode = ImuMode::IdleArmed;
    }
  } else {
    if (s_mode != ImuMode::IdleDisarmed) {
      board::sleep::disarm_light_sleep_imu();
      board::imu::disarm_wake_on_motion();
      s_mode = ImuMode::IdleDisarmed;
    }
  }
}

void apply_poll_mode() {
  if (s_mode == ImuMode::Poll) return;
  board::sleep::disarm_light_sleep_imu();
  // WoM arm が 2G/LP 設定を残すので通常値へ戻す。
  board::imu::disarm_wake_on_motion();
  s_mode = ImuMode::Poll;
}

// RaiseDetected → Wake Action (画面OFF時のみ)。引っ張ると画面が絶対
// 消えない歩行中の誤点灯を防ぐため、ScreenOff 系の時だけ変換する。
void raise_cb(const watch::Event&, void*) {
  const watch::PowerState s = power().state();
  if (s != watch::PowerState::ScreenOff &&
      s != watch::PowerState::DeepSleepCandidate) {
    return;
  }
  if (!settings_mut().raise_to_wake) return;
  watch::Action a{};
  a.type = watch::ActionType::Wake;
  a.source = watch::ActionSource::Imu;
  watch_app::push_action(a);
}

void notify_task(const watch::Event&, void*) {
  if (s_task) xTaskNotifyGive(s_task);
}

void imu_task(void*) {
  s_last_cause = esp_sleep_get_wakeup_cause();
  for (;;) {
    if (screen_on()) {
      apply_poll_mode();
      int16_t x, y, z;
      if (read_sample(&x, &y, &z)) push_sample(x, y, z);
      maybe_sync_hw();
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kPollMs));
    } else {
      apply_idle_mode();
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kIdleCheckMs));
      // light sleep から戻った等でループが回った時に同期判定。
      maybe_sync_hw();
      if (s_mode == ImuMode::IdleArmed && gpio_woke()) {
        burst();
      }
    }
  }
}

}  // namespace

void imu_service_start() {
  // ハード歩数計を主経路にする。失敗時はソフト検出 (core StepDetector)
  // が自動的に効く (StepsHwSync が一切飛ばないので feature が sw で数える)。
  if (board::imu::hw_pedometer_init() == ESP_OK) {
    s_hw_ped = true;
    // hw 経路有効を core に伝える (arg0=カウンタ生値、初回は差分0相当)。
    uint32_t cnt = 0;
    board::imu::hw_pedometer_steps(&cnt);
    s_next_hw_sync_ms = 0;
    maybe_sync_hw();
    // 画面OFFの light sleep でも 5分周期で起きて同期できるよう
    // RTC timer wake を常時有効にする (deep sleep 突入時は
    // sleep::enter_deep_sleep 側で解除する)。
    // TODO(hw): 5分周期は仮置き。消費電力と見て調整
    esp_sleep_enable_timer_wakeup(kHwRtcSyncUs);
    ESP_LOGI(TAG, "hw pedometer primary (cnt=%lu)", (unsigned long)cnt);
  } else {
    ESP_LOGW(TAG, "hw pedometer unavailable -> software fallback");
  }

  // RaiseDetected は app-task コンテキストで届く (EventBus の購読)。
  bus().subscribe(watch::EventType::RaiseDetected, raise_cb, nullptr);
  bus().subscribe(watch::EventType::PowerStateChanged, notify_task, nullptr);
  bus().subscribe(watch::EventType::SettingsChanged, notify_task, nullptr);
  const BaseType_t ok =
      xTaskCreate(imu_task, "imu_svc", 4096, nullptr, 4, &s_task);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "xTaskCreate failed");
  }
}

}  // namespace watch_app
