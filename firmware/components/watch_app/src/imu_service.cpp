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

#include <cstdio>

#include "board/board.hpp"
#include "driver/gpio.h"
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
// WoM は動作開始時点で発火するので直後の ~300ms は全サンプルが
// 動的 (|a| が 0.6-1.7g 範囲外) で RaiseDetector に棄却される。
// 「上げて静止」した姿勢を取り込むには ~1.2s の窓が必要。
constexpr uint32_t kBurstN = 24;      // GPIO起床後の連続サンプル数
constexpr uint32_t kBurstMs = 50;     // 24 × 50ms = 1.2s 窓
constexpr uint32_t kHwSyncPollMs = 2000;    // 画面ON中の歩数同期間隔
constexpr uint64_t kHwRtcSyncUs = 5ull * 60 * 1000000;  // 画面OFF中5分周期

TaskHandle_t s_task = nullptr;
bool s_hw_ped = false;          // ハード歩数計が有効か
bool s_hw_counted = false;      // STEP_CNT が実際に増えたか
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
    // カウンタが実際に増えたかを見る。増えていない間は HW を主経路と
    // みなさず、ソフト検出用に 20Hz サンプリングを続ける。
    static uint32_t s_last_cnt = 0;
    if (cnt != s_last_cnt) {
      if (cnt != 0) s_hw_counted = true;
      s_last_cnt = cnt;
      ESP_LOGI(TAG, "STEP_CNT=%lu", (unsigned long)cnt);
    }
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
// light sleep には入っていない (esp_pm 設定で無効) ので、WoM の INT1
// (GPIO21) を通常の GPIO 割り込みで受けて burst を起こす。
// WoM INT (INT1) は STATUS1 読取まで保持されるレベル出力。ISR は
// フラグを立ててタスクへ通知するだけにし、タスク側で STATUS1 を
// 読んでラッチ解除 + イベント有無を判定する。

enum class ImuMode : uint8_t { Poll, IdleArmed, IdleDisarmed };
ImuMode s_mode = ImuMode::Poll;

volatile bool s_wom_seen = false;
// deep sleep 移行中は true — app 側が入眠用に arm した WoM 設定を
// このタスクが通常 arm へ上書きするのを防ぐ (init_high 診断で実害あり)。
volatile bool s_ds_freeze = false;

void IRAM_ATTR wom_isr(void*) {
  s_wom_seen = true;
  if (!s_task) return;
  BaseType_t woken = pdFALSE;
  vTaskNotifyGiveFromISR(s_task, &woken);
  if (woken) portYIELD_FROM_ISR();
}

void burst() {
  // WoM arm 中は加速度データが WoM コンパレータ専用になり出力レジスタが
  // 凍結する (実機観測: 全サンプル同一値・range も WoM 用に残る)。
  // 起床ジェスチャの姿勢は通常ストリーミングに戻してから取る。
  board::imu::disarm_wake_on_motion();
  // 起床直後の向きを core の RaiseDetector に流す。
  int16_t x = 0, y = 0, z = 0;
  // TEMP(hw-test): 検出可否の切り分け用に全サンプルを1行に出す。
  // RaiseDetector 確定後に削って良い。
  char line[8 + kBurstN * 24];
  int n = snprintf(line, sizeof(line), "burst:");
  for (uint32_t i = 0; i < kBurstN; ++i) {
    if (read_sample(&x, &y, &z)) {
      push_sample(x, y, z);
      if (n + 22 < (int)sizeof(line)) {
        n += snprintf(line + n, sizeof(line) - n, " %d,%d,%d", x, y, z);
      }
    }
    if (i + 1 < kBurstN) vTaskDelay(pdMS_TO_TICKS(kBurstMs));
  }
  ESP_LOGI(TAG, "%s", line);
  // まだ消灯中なら再 arm (点灯していたら apply_poll_mode 側で管理)。
  // ds_freeze 中は入眠用 arm を上書きしない。
  if (!screen_on() && !s_ds_freeze) {
    board::imu::arm_wake_on_motion();
    s_wom_seen = false;  // arm 直後の残留イベントは捨てる
  }
}

void apply_idle_mode() {
  if (settings_mut().raise_to_wake) {
    if (s_mode != ImuMode::IdleArmed) {
      board::imu::arm_wake_on_motion();
      s_wom_seen = false;
      gpio_intr_enable(GPIO_NUM_21);  // WoM INT (ANYEDGE)
      // arm 中に既に HIGH まで上がっている (割り込み有効化前に発火した
      // イベントがラッチ済み) ケースをエッジ待ちせず拾う。
      if (gpio_get_level(GPIO_NUM_21)) s_wom_seen = true;
      s_mode = ImuMode::IdleArmed;
    }
  } else {
    if (s_mode != ImuMode::IdleDisarmed) {
      gpio_intr_disable(GPIO_NUM_21);
      s_wom_seen = false;
      board::imu::disarm_wake_on_motion();
      s_mode = ImuMode::IdleDisarmed;
    }
  }
}

void apply_poll_mode() {
  // QMI8658 の WoM arm は ESP 側リセット (deep sleep 復帰含む) を跨いで
  // チップ内に持続し、arm 中は出力レジスタが凍結する (実機観測)。
  // s_mode はこのブートでは Poll 初期値だがチップ側は arm のままかも
  // しれないので、起動後最初の Poll 移行では必ず disarm する。
  static bool s_streamed = false;  // 本ブートで一度でも通常化したか
  if (s_mode == ImuMode::Poll && s_streamed) return;
  gpio_intr_disable(GPIO_NUM_21);
  s_wom_seen = false;
  // WoM arm が 2G/LP 設定を残すので通常値へ戻す。
  board::imu::disarm_wake_on_motion();
  s_streamed = true;
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
  ESP_LOGI(TAG, "raise detected -> wake");
  watch::Action a{};
  a.type = watch::ActionType::Wake;
  a.source = watch::ActionSource::Imu;
  watch_app::push_action(a);
}

void notify_task(const watch::Event&, void*) {
  if (s_task) xTaskNotifyGive(s_task);
}

void imu_task(void*) {
  for (;;) {
    if (screen_on()) {
      apply_poll_mode();
      // hw_ped 時はソフト歩数は不要だが、RaiseDetector の基準ベクトル
      // (普段の向き) は ImuSample を流さないと学習されない — 画面OFF中の
      // burst だけだと「1サンプル目が基準=上げた向き」になり角度差が
      // 出ず検出が死ぬ。2Hz の trickle で基準を追従させる
      // (SW フォールバック時は歩数計数用に 20Hz 継続)。
      static int s_poll_div = 0;
      // HW が実際に数え始めるまでは 20Hz を維持する (エンジンが動かない
      // 個体では core のソフト検出が歩数を拾う)。数え始めたら 2Hz に落とす。
      const int div = (s_hw_ped && s_hw_counted) ? 10 : 1;  // 50ms × 10
      if (++s_poll_div >= div) {
        s_poll_div = 0;
        int16_t x, y, z;
        if (read_sample(&x, &y, &z)) push_sample(x, y, z);
      }
      maybe_sync_hw();
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kPollMs));
    } else {
      // deep sleep 移行中は IMU への一切の書き込みを止める — app 側が
      // arm した WoM (init_high 等) を apply_idle_mode/burst/STATUS1
      // 読取で上書き・解除すると入眠直前の INT1 極性が反転する。
      if (s_ds_freeze) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kIdleCheckMs));
        continue;
      }
      apply_idle_mode();
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kIdleCheckMs));
      // ループが回った時に歩数同期判定 (kHwRtcSyncUs 相当の間隔で届く)。
      maybe_sync_hw();
      // WoM 割り込み (ISR → s_wom_seen)。INT1 は STATUS1 読取まで
      // ラッチされるので、エッジを取りこぼした (=arm 直前/直後に
      // 発火して既に HIGH) ケースもレベル読みで拾う。
      if (s_mode == ImuMode::IdleArmed) {
        const bool pending = s_wom_seen || gpio_get_level(GPIO_NUM_21);
        s_wom_seen = false;
        if (pending && !screen_on()) {
          uint8_t s1 = 0;
          // STATUS1.bit2 (WoM) だけを見る。bit4 は Pedometer イベントで
          // 歩行中にも立つ — これを起床と誤ると歩くたび burst が走る。
          if (board::imu::wom_status(&s1) == ESP_OK && (s1 & 0x04)) {
            ESP_LOGI(TAG, "imu int s1=%02x", s1);
            burst();
          }
        }
      }
    }
  }
}

}  // namespace

void imu_service_freeze_for_deep_sleep() {
  s_ds_freeze = true;
}

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

  // WoM INT (GPIO21) を GPIO 割り込みで受ける。INT1 は STATUS1 読取まで
  // ラッチされる (実機確認済み)。ANYEDGE + タスク側のレベル読みで
  // 「割り込み有効化前に既に HIGH」の取りこぼしも拾う
  // (POSEDGE だけだと既発火イベントを永久に逃す)。
  // 有効化/無効化自体は IdleArmed/Poll のモード遷移側で行う。
  // ISR サービスは touch/button ドライバが先に入れることが多い。
  // 二重インストール時に gpio ドライバ内部が E ログを出すので、
  // 呼び出し中だけ "gpio" タグを黙らせる (INVALID_STATE は正常)。
  const esp_log_level_t gpio_lvl = esp_log_level_get("gpio");
  esp_log_level_set("gpio", ESP_LOG_NONE);
  const esp_err_t isr_ret = gpio_install_isr_service(0);
  esp_log_level_set("gpio", gpio_lvl);
  if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "isr service: %s", esp_err_to_name(isr_ret));
  }
  gpio_config_t icfg = {};
  icfg.pin_bit_mask = 1ULL << GPIO_NUM_21;
  icfg.mode = GPIO_MODE_INPUT;
  icfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  icfg.pull_up_en = GPIO_PULLUP_DISABLE;
  icfg.intr_type = GPIO_INTR_ANYEDGE;  // INT1 はイベント毎に反転/ラッチされる
  gpio_config(&icfg);
  gpio_intr_disable(GPIO_NUM_21);
  const esp_err_t add_ret = gpio_isr_handler_add(GPIO_NUM_21, wom_isr, nullptr);
  if (add_ret != ESP_OK) {
    ESP_LOGW(TAG, "isr add failed: %s", esp_err_to_name(add_ret));
  }
}

}  // namespace watch_app
