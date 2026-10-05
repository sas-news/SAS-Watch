// SAS-Watch board layer: Waveshare ESP32-S3-Touch-AMOLED-2.06 の物理層
// docs/plan.md A 章 / docs/board.md の値を使う。推測値には TODO(hw) を付ける。
#pragma once

#include <stdint.h>
#include <time.h>
#include "esp_err.h"

namespace board {

enum class Button : uint8_t {
    Boot = 0,  // GPIO0, 押下で LOW  (RTC GPIO)
    Pwr  = 1,  // GPIO10, 押下で HIGH (RTC GPIO, AXP2101 SYS_OUT)
};

enum class ButtonEvent : uint8_t {
    ShortPress,   // 短押し (single click)
    LongPress,    // 長押し 800ms
    DoubleClick,  // 2回押し
};

using ButtonCallback = void (*)(Button button, ButtonEvent event);

// bsp_i2c_init -> pmic -> rtc -> imu -> buttons -> haptics の順で初期化。
// 個別デバイスの失敗はログに出して続行する (実機なし前提の開発用)。
esp_err_t init();

namespace buttons {
    esp_err_t init();
    // cb はボタンライブラリのタスクコンテキストから呼ばれる。LVGL を触るなら
    // bsp_display_lock() が必要。NULL で解除。
    void set_callback(ButtonCallback cb);
}

namespace pmic {
    esp_err_t init();
    int  battery_percent();   // 0-100, 取得失敗時 -1
    bool is_charging();
    bool is_vbus();           // USB 電源が入っているか
    // AXP2101 ALDO2 = DSI_PWR_EN (パネル電源)。
    // 切ると画面の再初期化が必要 // TODO(hw): 実機で再初期化時間を確認
    void panel_power(bool on);
}

namespace rtc {
    esp_err_t init();
    // 起動時に RTC の時刻をシステム時刻へ反映する
    esp_err_t apply_to_system_time();
    esp_err_t get_epoch(time_t* out);   // RTC が読めなければエラー
    esp_err_t set_epoch(time_t t);      // UTC epoch を RTC に書く
    // アラーム (時・分・秒に一致 = 毎日その時刻。次回発火を都度書き込む前提)。
    // INT (GPIO39) が LOW になるので light sleep wake として使える。
    // deep sleep からは復帰できないので deep sleep 中は RTC timer wakeup で起きる。
    esp_err_t set_alarm_epoch(time_t t);
    // アラーム割り込み解除 (AIE off + AF clear)。発火後の INT LOW 解除にも使う。
    esp_err_t clear_alarm();
}

namespace imu {
    esp_err_t init();
    esp_err_t read_accel_mg(float* x, float* y, float* z);
    // wake-on-motion を ARM して GPIO21 (INT1) で復帰可能にする準備。
    // 実際の deep sleep wake 設定は sleep::arm_deep_sleep_wake() で行う。
    esp_err_t arm_wake_on_motion();
}

namespace haptics {
    esp_err_t init();
    void pulse(uint32_t ms);  // GPIO18 駆動パルス。モーター無しでも害なし // TODO(hw): モーター有無
}

namespace sleep {
    // light sleep 用 GPIO wake: GPIO38(タッチ INT, LOW) / GPIO0(BOOT, LOW) / GPIO10(PWR, HIGH)
    void arm_light_sleep_wake();
    // deep sleep wake: BOOT=ext0(GPIO0 LOW), PWR/IMU=ext1(GPIO10|GPIO21 ANY_HIGH),
    // wake_after_us > 0 なら timer wake も有効化。戻らない。
    void enter_deep_sleep(uint64_t wake_after_us) __attribute__((noreturn));
}

}  // namespace board
