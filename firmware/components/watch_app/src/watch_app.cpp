// watch_app.cpp — app タスク本体。
//   キューを1件処理 → tick → power_apply → 眠る (notify か deadline で起床)。
//   DeepSleepCandidate で queue 空なら保存して board::enter_deep_sleep。
#include "watch_app/watch_app.hpp"

#include <unistd.h>

#include "internal.hpp"

#include "audio/audio.hpp"
#include "board/board.hpp"
#include "ota/ota.hpp"
#include "wifi/wifi.hpp"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "power_apply.hpp"
#include "ui/port.hpp"
#include "ui/ui.hpp"
#include "watch/features/alarm.hpp"
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

// deep sleep を跨ぐ診断 (RTC_DATA_ATTR — RTC RAM は電源維持)。
struct SleepDiag {
  uint32_t magic;
  int32_t wom_ret;
  int32_t imu_lvl_after_arm;
  int32_t imu_lvl_after_panel_off;
  int32_t imu_rtc_lvl;  // RTC ドメインから見た GPIO21 他の probe ビット
  uint32_t rtc_regs[4];  // {PAD21,PAD10,HOLD,IN} 生レジスタ
  uint64_t wake_us;
};
RTC_DATA_ATTR SleepDiag s_sleep_diag;
constexpr uint32_t kSleepDiagMagic = 0x5A5A0001;

// USB-Serial/JTAG の 's' 入力で強制 deep sleep できるデバッグ経路。
// 自動深眠 (wants_deep_sleep) とは独立に常時有効 (serial_dbg_task)。
volatile bool s_serial_sleep_req = false;

void enter_deep_sleep() {
  ESP_LOGI(TAG, "deep sleep");
  s_features.save_all(*s_fctx);
  // feature の期限だけを見る。s_rt.next_deadline_ms() は PowerPolicy の
  // 画面遷移時刻 (Dim 8s 等) も min に入れるため、手動入眠では数秒で
  // タイマー起床してしまう (実測: 's' → +2s timer wake)。
  const int64_t dl = s_features.next_deadline_ms();
  const int64_t now = s_clock->now_ms();
  const uint64_t us = dl > now ? static_cast<uint64_t>(dl - now) * 1000 : 0;
  // 入眠側ログは USB-Serial/JTAG が先に死ぬと出ないので RTC データに残し、
  // 次回起動時にダンプする。
  s_sleep_diag.magic = kSleepDiagMagic;
  s_sleep_diag.wake_us = us;
  s_sleep_diag.wom_ret = -999;
  // 起床: PWR(ext1) / 次の期限でタイマー / raise_to_wake なら IMU INT1。
  // BOOT(GPIO0) は strapping pin なので deep sleep 復帰には使えない
  // (押下で LOW がサンプルされ download mode に落ちる → enter_deep_sleep 参照)。
  if (s_settings.raise_to_wake) {
    // imu_task がこの後の窓で WoM arm を上書きしないよう先に凍結する
    // (burst 末尾の通常 arm や STATUS1 読みで INT1 極性が反転するのを防ぐ)。
    imu_service_freeze_for_deep_sleep();
    // 起床ゲート (maybe_fast_resleep) 用の基準姿勢を採る。WoM arm 中は
    // 出力レジスタが凍結するので、一度解除して 1 サンプル取ってから
    // arm する (LP 21Hz ≈ 48ms/サンプル)。
    board::imu::disarm_wake_on_motion();
    vTaskDelay(pdMS_TO_TICKS(80));
    float rx = 0, ry = 0, rz = 0;
    const esp_err_t rr = board::imu::read_accel_mg(&rx, &ry, &rz);
    if (rr == ESP_OK) {
      board::sleep::set_wake_reference(rx, ry, rz, us);
    } else {
      ESP_LOGW(TAG, "wake ref read failed: %s", esp_err_to_name(rr));
    }
    const esp_err_t r = board::imu::arm_wake_on_motion(false);
    s_sleep_diag.wom_ret = r;
    // arm 中/直後に発火したイベントが INT1 を HIGH にラッチしたままだと
    // ext1 (ANY_HIGH) が入眠直後に即発火する。ピンが 0 に落ちるまで
    // STATUS1 読取 (=ラッチ解除) を数回繰り返す。
    for (int i = 0; i < 4 && gpio_get_level(GPIO_NUM_21); ++i) {
      board::imu::wom_clear();
      vTaskDelay(pdMS_TO_TICKS(5));
    }
    s_sleep_diag.imu_lvl_after_arm = gpio_get_level(GPIO_NUM_21);
    ESP_LOGI(TAG, "wom arm: %s", esp_err_to_name(r));
    board::sleep::log_pin_levels(TAG);  // arm 直後の INT1 実レベル (0 が正常)
  }
  vTaskDelay(pdMS_TO_TICKS(50));
  s_sleep_diag.imu_lvl_after_panel_off = gpio_get_level(GPIO_NUM_21);
  board::sleep::enter_deep_sleep(us, s_settings.raise_to_wake != 0,
                                 &s_sleep_diag.imu_rtc_lvl,
                                 s_sleep_diag.rtc_regs);
}

// core が計算した次回アラーム発火時刻を PCF85063 のアラームに反映する。
// 値が変わった時だけ I2C 書き込み (軽量化)。鳴動に入ったら AF をクリアして
// INT (GPIO39) を HIGH に戻す — LOW のままだと light sleep が即起きてしまう。
int64_t s_rtc_alarm = -1;   // RTC に書いた最後の発火時刻 (-1 = 未同期)
#if SAS_APP_ALARM
bool s_af_cleared = false;  // 鳴動中に AF を1度だけクリアしたか
#endif
void alarm_rtc_sync() {
#if SAS_APP_ALARM
  if (watch::features::alarm_ringing()) {
    if (!s_af_cleared) {
      board::rtc::clear_alarm();
      s_af_cleared = true;
    }
    // 鳴動が終わったら必ず次回を再書き込みさせる。
    s_rtc_alarm = -1;
    return;
  }
  s_af_cleared = false;
  const int64_t e = watch::features::alarm_next_fire_epoch();
  if (e == s_rtc_alarm) return;
  s_rtc_alarm = e;
  if (e > 0) {
    const esp_err_t ret = board::rtc::set_alarm_epoch(static_cast<time_t>(e));
    if (ret != ESP_OK) {
      ESP_LOGW(TAG, "rtc alarm write: %s", esp_err_to_name(ret));
    }
  } else {
    board::rtc::clear_alarm();
  }
#else
  // アラームアプリ無し: RTC のアラームは起動時に1度だけ無効化する。
  if (s_rtc_alarm != 0) {
    s_rtc_alarm = 0;
    board::rtc::clear_alarm();
  }
#endif
}

volatile uint32_t s_app_loops = 0;  // app_task 心拍 (serial 'h' で観測)

void app_task(void*) {
  for (;;) {
    s_app_loops++;
    ble_glue_poll();
    ota::poll();  // OTA 進捗の Event 化 + 電源 Lease (deep sleep 禁止)

    const int64_t now = s_clock->now_ms();
    // タッチの活動は LVGL 側の計測を活動時刻へ反映する。
    // 行タップやスクロールのように Action 化されない入力でも
    // 無操作タイマーをリセットするため。
    // ScreenOff 中は lv_timer が止まり LVGL 側の時刻が進まないので、
    // touched が last_activity を越えるのは新しい入力があった時だけ
    // (強制消灯がタッチで解除されることはない)。
    int64_t touched = 0;
    if (ui::port::lock(50)) {
      touched = now - static_cast<int64_t>(
                          lv_display_get_inactive_time(nullptr));
      ui::port::unlock();
    }
    core_lock();
    if (touched > s_power.last_activity_ms()) {
      s_power.kick_activity(touched);
    }
    s_rt.step(now, *s_fctx);
    // core 状態を読むので lock の内側。I2C 書き込みは値が変わった時だけ。
    alarm_rtc_sync();
    // settings.set (BLE) で変わった Double 割り当てをボタン即時化に反映。
    input_sync_double_enabled();
    core_unlock();
    power_apply(s_power.state(), s_settings);

    // 入眠は自動 (wants_deep_sleep) または USB デバッグ 's' 入力。
    // 's' は強制なのでキュー残があっても眠る。自動パスは未処理 Action を
    // 全部捌いてからにするためキュー空を待つ — 詰まったままだと深眠でき
    // ないので滞留時は1回だけ中身を出す。
    const bool want_ds =
        s_serial_sleep_req || s_power.wants_deep_sleep();
    if (want_ds) {
      if (s_serial_sleep_req || s_rt.queue().empty()) {
        enter_deep_sleep();  // 戻らない
      } else {
        static int64_t s_qstuck_log = -1;
        if (now - s_qstuck_log > 5000) {
          s_qstuck_log = now;
          ESP_LOGW(TAG, "deep sleep blocked: action queue not empty");
        }
      }
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
    case ESP_SLEEP_WAKEUP_EXT0: s = "ext0 (IMU)"; break;
    case ESP_SLEEP_WAKEUP_EXT1: s = "ext1 (PWR/IMU)"; break;
    case ESP_SLEEP_WAKEUP_TIMER: s = "timer"; break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD: s = "touchpad"; break;
    case ESP_SLEEP_WAKEUP_ULP: s = "ulp"; break;
    case ESP_SLEEP_WAKEUP_GPIO: s = "gpio"; break;
    case ESP_SLEEP_WAKEUP_UART: s = "uart"; break;
    default: s = "power-on/reset"; break;
  }
  ESP_LOGI(TAG, "wake reason: %s", s);
  if (c == ESP_SLEEP_WAKEUP_EXT1) {
    // ext1 は発火ピンのマスクが取れる (BIT(10)=PWR / BIT(21)=IMU)。
    ESP_LOGI(TAG, "ext1 pins: 0x%llx", static_cast<unsigned long long>(
                                        esp_sleep_get_ext1_wakeup_status()));
  }
  if (c == ESP_SLEEP_WAKEUP_GPIO || c == ESP_SLEEP_WAKEUP_EXT1) {
    board::sleep::log_pin_levels(TAG);
  }
  // 前回入眠時に RTC RAM へ残した診断を出す (入眠ログは USB が先に切れて
  // 届かないことがあるため)。
  if (s_sleep_diag.magic == kSleepDiagMagic) {
    ESP_LOGI(TAG,
             "sleep diag: wom_ret=%ld imu_lvl=%ld imu_off_lvl=%ld rtc_lvl=%ld wake_us=%llus",
             static_cast<long>(s_sleep_diag.wom_ret),
             static_cast<long>(s_sleep_diag.imu_lvl_after_arm),
             static_cast<long>(s_sleep_diag.imu_lvl_after_panel_off),
             static_cast<long>(s_sleep_diag.imu_rtc_lvl),
             static_cast<unsigned long long>(s_sleep_diag.wake_us / 1000000));
    ESP_LOGI(TAG, "sleep regs: p21=%08lx p10=%08lx hold=%08lx in=%08lx",
             static_cast<unsigned long>(s_sleep_diag.rtc_regs[0]),
             static_cast<unsigned long>(s_sleep_diag.rtc_regs[1]),
             static_cast<unsigned long>(s_sleep_diag.rtc_regs[2]),
             static_cast<unsigned long>(s_sleep_diag.rtc_regs[3]));
  }
  int32_t gate_n = 0, gate_pass = -1;
  float gate_deg = 0, gate_dir[3] = {0, 0, 0};
  if (board::sleep::last_gate_debug(&gate_n, &gate_deg, gate_dir,
                                    &gate_pass)) {
    ESP_LOGI(TAG, "imu gate: n=%ld deg=%.1f dir=%.1f,%.1f,%.1f pass=%ld",
             static_cast<long>(gate_n), gate_deg, gate_dir[0], gate_dir[1],
             gate_dir[2], static_cast<long>(gate_pass));
  }
  int ent_i = 0, ent_l = 0;
  if (board::sleep::last_entry_debug(&ent_i, &ent_l)) {
    // iters<20 なら LOW 確認して寝た、20 なら打切り (HIGH のまま入眠)。
    ESP_LOGI(TAG, "sleep entry: still_iters=%d int1=%d", ent_i, ent_l);
  }
}

// USB-Serial/JTAG からのデバッグ入力 + 起床診断の再出力。
// ブート +6s で wake reason/diag を再出 (モニタ再接続遅延で最初のログを
// 逃すため)。その後は 's' 入力をポーリング → 強制 deep sleep。
// stdin は USJ no-driver vfs なので read は FIFO 空なら即 0 を返す
// (非ブロッキング)。ドライバ版が入る環境でも read_bytes(timeout=0) 同等。
// 1タスクに統合しているのはヒープ節約のため (xTaskCreate が実際に
// メモリ不足で失敗した)。
void serial_dbg_task(void*) {
  vTaskDelay(pdMS_TO_TICKS(6000));
  log_wake_reason();
  // ブート直後の STATUS1 (深眠中に WoM 発火したかの確認用)。bit2=WoM。
  ESP_LOGI(TAG, "boot_s1=0x%02X", board::imu::boot_status1_raw());
  s_sleep_diag.magic = 0;
  for (;;) {
    char buf[32];
    const ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
    for (ssize_t i = 0; i < n; ++i) {
      if (buf[i] == 's' || buf[i] == 'S') {
        s_serial_sleep_req = true;
        ESP_LOGI(TAG, "serial: deep sleep requested ('s')");
        wake_task();  // app タスクが ulTaskNotifyTake 待機なら起こす
      } else if (buf[i] == 'r' || buf[i] == 'R') {
        // raise_to_wake を ON にする (タッチが効かない時の救出用)。
        watch::Action a;
        a.type = watch::ActionType::SetRaiseToWake;
        a.arg0 = 1;
        push_action(a);
        ESP_LOGI(TAG, "serial: raise_to_wake=1 requested ('r')");
        wake_task();
      } else if (buf[i] == 'd' || buf[i] == 'D') {
        // NVS の電源系設定を既定値に戻す ('d'efaults)。
        // 古いデバッグ値 (deep_sleep_after_s=60 等) が残っていると
        // 既定値変更が効かないので直接書き戻す。
        watch::settings_set_u32(s_settings, s_kv, "deep_sleep_after_s", 1800);
        watch::settings_set_u32(s_settings, s_kv, "usb_keep_awake", 1);
        watch::settings_set_u32(s_settings, s_kv, "raise_to_wake", 1);
        core_lock();
        s_power.configure(s_settings.dim_after_s, s_settings.screen_off_after_s,
                          s_settings.deep_sleep_after_s);
        s_power.set_usb_keep_awake(s_settings.usb_keep_awake);
        core_unlock();
        ESP_LOGI(TAG, "serial: power settings reset to defaults ('d')");
        wake_task();
      } else if (buf[i] == 'g' || buf[i] == 'G') {
        // 直前の起床ゲート/入眠診断を再出 ('g'ate) — +6s ダンプを
        // 逃しても読める。
        int32_t gn = 0, gpass = -1;
        float gd = 0, gdir[3] = {0, 0, 0};
        if (board::sleep::last_gate_debug(&gn, &gd, gdir, &gpass)) {
          ESP_LOGI(TAG, "serial: gate n=%ld deg=%.1f dir=%.1f,%.1f,%.1f pass=%ld",
                   static_cast<long>(gn), gd, gdir[0], gdir[1], gdir[2],
                   static_cast<long>(gpass));
        } else {
          ESP_LOGI(TAG, "serial: gate (none)");
        }
        int ei = 0, el = 0;
        if (board::sleep::last_entry_debug(&ei, &el)) {
          ESP_LOGI(TAG, "serial: entry still_iters=%d int1=%d", ei, el);
        }
      } else if (buf[i] == 'h' || buf[i] == 'H') {
        // app_task 心拍 + キュー状態を出す ('h'ealth)。
        ESP_LOGI(TAG, "serial: app_loops=%lu sleep_req=%d q_empty=%d heap=%lu",
                 static_cast<unsigned long>(s_app_loops),
                 (int)s_serial_sleep_req,
                 (int)s_rt.queue().empty(),
                 static_cast<unsigned long>(esp_get_free_internal_heap_size()));
      } else if (buf[i] == 'a' || buf[i] == 'A') {
        // 生の加速度を出す ('a'ccel) — 起床ゲート/腕上げ判定の生値確認用。
        // 画面OFF中は WoM arm で出力が凍結しているので、一瞬解除して
        // 生値を採ってから戻す (LP 21Hz ≈ 48ms/サンプル)。
        const watch::PowerState ps = s_power.state();
        const bool armed = ps == watch::PowerState::ScreenOff ||
                           ps == watch::PowerState::DeepSleepCandidate;
        if (armed) {
          board::imu::disarm_wake_on_motion();
          vTaskDelay(pdMS_TO_TICKS(80));
        }
        float ax = 0, ay = 0, az = 0;
        const esp_err_t r = board::imu::read_accel_mg(&ax, &ay, &az);
        if (armed) {
          board::imu::arm_wake_on_motion(false);
        }
        ESP_LOGI(TAG, "serial: accel %s x=%.0f y=%.0f z=%.0f mg",
                 esp_err_to_name(r), ax, ay, az);
        // PWR の実極性確認用 (待機 HIGH なら押下=LOW で ext1 ANY_HIGH は
        // 構造的に発火しない)。INT1(GPIO21) も併記 — 静止時に HIGH の
        // ままなら wom_clear が効いていない。
        ESP_LOGI(TAG, "serial: pwr=%d int1=%d",
                 (int)gpio_get_level(GPIO_NUM_10),
                 (int)gpio_get_level(GPIO_NUM_21));
      } else if (buf[i] == 'c' || buf[i] == 'C') {
        // WoM ラッチをクリアして INT1 の応答を見る ('c'lear)。
        // clear 後に LOW へ落ちれば clear は有効 (HIGH 継続なら再発火中か
        // ピンがラッチ解除されない)。
        const esp_err_t cr = board::imu::wom_clear();
        vTaskDelay(pdMS_TO_TICKS(60));
        ESP_LOGI(TAG, "serial: wom_clear=%s int1=%d",
                 esp_err_to_name(cr), (int)gpio_get_level(GPIO_NUM_21));
      } else if (buf[i] == 'b' || buf[i] == 'B') {
        // 描画ベンチ ('b'ench): 主要画面へ順に遷移して ui::perf_bench。
        // 画面遷移は Navigate Action (タッチ操作と同じ経路)。
        static constexpr watch::Route kRoutes[] = {
            watch::Route::Home,     watch::Route::More,
            watch::Route::Settings, watch::Route::Stopwatch,
            watch::Route::Timer,    watch::Route::Notifications,
        };
        ESP_LOGI(TAG, "serial: bench start ('b')");
        for (watch::Route r : kRoutes) {
          watch::Action a;
          a.type = r == watch::Route::Home ? watch::ActionType::Home
                                           : watch::ActionType::Navigate;
          // source=System だと kick_activity されず bench 中に画面が
          // 消えて後半が計測できない。タッチ操作として扱う。
          a.source = watch::ActionSource::Touch;
          a.arg0 = static_cast<uint32_t>(r);
          push_action(a);
          wake_task();
          vTaskDelay(pdMS_TO_TICKS(1200));
          ui::perf_bench();
          if (r != watch::Route::Home) {
            watch::Action h;
            h.type = watch::ActionType::Home;
            h.source = watch::ActionSource::Touch;
            push_action(h);
            wake_task();
            vTaskDelay(pdMS_TO_TICKS(800));
          }
        }
        ESP_LOGI(TAG, "serial: bench done");
      } else if (buf[i] == 'p' || buf[i] == 'P') {
        // PNG デコーダ比較テスト ('p'ngtest): 行ストリーム版と
        // lodepng 版の出力をバイト一致で検証する (LVGL タスク上)。
        ui::png_selftest();
      } else if (buf[i] == 't' || buf[i] == 'T') {
        // テーマ巡回 ('t'heme): 同梱テーマを順に SetTheme で適用する。
        // ble_glue の pending 経路と同じく Action の text に id を積む。
        static const char* kThemes[] = {"standard", "mame", "cosmos",
                                        "cute", "cyber"};
        static int s_theme_idx = 1;  // 現在 standard なので次から
        const char* id = kThemes[s_theme_idx++ %
                                 (sizeof(kThemes) / sizeof(kThemes[0]))];
        watch::Action a;
        a.type = watch::ActionType::SetTheme;
        a.source = watch::ActionSource::Touch;
        a.set_text(id);
        push_action(a);
        wake_task();
        ESP_LOGI(TAG, "serial: theme -> %s", id);
      } else if (buf[i] == 'w' || buf[i] == 'W') {
        // WoM 再 arm — 初期値に強制できるかの切り分け ('w'om rearm)。
        board::imu::disarm_wake_on_motion();
        const esp_err_t ar = board::imu::arm_wake_on_motion(false);
        vTaskDelay(pdMS_TO_TICKS(60));
        ESP_LOGI(TAG, "serial: rearm=%s int1=%d",
                 esp_err_to_name(ar), (int)gpio_get_level(GPIO_NUM_21));
      }
    }
    vTaskDelay(pdMS_TO_TICKS(150));
  }
}

}  // namespace

bool start(const Deps& deps) {
  if (s_started || !deps.clock || !deps.kv) return false;
  s_clock = deps.clock;
  s_kv = deps.kv;
  s_core_mtx = xSemaphoreCreateMutex();

  log_wake_reason();
  // 起床診断の再出力 + シリアル 's' → 強制 deep sleep のデバッグタスク。
  // 2048B では 'd' の NVS 書き込みパスでスタックオーバーフローした実績が
  // あるため 4KB。動的確保だと内蔵 RAM 断片化で作れないことがあるので
  // app_task と同じく静的確保。
  static StaticTask_t s_sdbg_tcb;
  static StackType_t s_sdbg_stack[4096 / sizeof(StackType_t)];
  if (!xTaskCreateStatic(serial_dbg_task, "sdbg", 4096, nullptr, 1,
                         s_sdbg_stack, &s_sdbg_tcb)) {
    ESP_LOGE(TAG, "sdbg task create failed");
  }
  // ヒープ余量 (sdbg 追加で app_task 作成が落ちたことがあるため監視用)。
  ESP_LOGI(TAG, "heap free=%lu internal=%lu",
           static_cast<unsigned long>(esp_get_free_heap_size()),
           static_cast<unsigned long>(
               esp_get_free_internal_heap_size()));

  // 設定の復元 → 電源しきい値・ボタン割り当てに反映。
  watch::settings_load(s_settings, *s_kv);
  s_power.configure(s_settings.dim_after_s, s_settings.screen_off_after_s,
                    s_settings.deep_sleep_after_s);
  s_power.set_usb_keep_awake(s_settings.usb_keep_awake);
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
  // Wi-Fi 資格情報 (NVS) + OTA セッション (セッション中だけ Wi-Fi を上げる)。
  wifi::init(s_kv);
  ota::init(&s_power, &s_bus);
  s_rt.init(fctx);
  s_features.restore_all(fctx);

  s_bus.subscribe(watch::EventType::BleConnChanged, ble_cb, nullptr);
  ble_glue_init();

  // UI の Action 出口をこのキューへ。
  ui::set_action_sink(&push_action);
  input_init(&s_input);

  // 起動時輝度 = 保存値。画面ONは ui::init_display 側。
  ui::port::brightness_apply(static_cast<int>(s_settings.brightness));

  // esp_pm: CPU 240MHz 固定・自動 light sleep 無効。
  // TODO(hw): 実機で確認 — 応答性優先で固定にしている。DFS (min=40MHz)
  // 復帰はレンダリング遅延とのトレードオフ、light sleep 復帰は
  // touch/panel の復帰確認が必要。
  const esp_pm_config_t pm_cfg = {
      .max_freq_mhz = 240,
      .min_freq_mhz = 240,
      .light_sleep_enable = false,
  };
  const esp_err_t pm_ret = esp_pm_configure(&pm_cfg);
  if (pm_ret != ESP_OK) {
    ESP_LOGW(TAG, "esp_pm_configure: %s", esp_err_to_name(pm_ret));
  }
  // light sleep からの起床を配線 (タッチ INT/BOOT/PWR)。
  board::sleep::arm_light_sleep_wake();

  // メインタスクは静的確保 — 動的 xTaskCreate は内蔵 RAM の断片化で
  // 確保に失敗し得る (実測: internal 21KB 残りでも 8KB スタックが
  // 連続確保できず起動不能になった)。
  static StaticTask_t s_app_tcb;
  static StackType_t s_app_stack[8192 / sizeof(StackType_t)];
  s_task = xTaskCreateStatic(app_task, "watch_app", 8192, nullptr, 5,
                             s_app_stack, &s_app_tcb);
  if (!s_task) {
    ESP_LOGE(TAG, "xTaskCreateStatic failed");
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

void set_external_power(bool on) {
  core_lock();
  s_power.set_external_power(on);
  core_unlock();
  // 変化を即座に遷移評価させる (VBUS 抜き → 抑制解除など)。
  wake_task();
}

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
