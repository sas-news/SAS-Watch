#include "board/board.hpp"
#include "pins.hpp"

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_check.h"
#include "driver/gpio.h"

// docs/board.md / plan.md A-3:
// - deep sleep から復帰できるのは PWR(GPIO10), IMU(GPIO21), timer だけ
//   (GPIO38 タッチ, GPIO39 RTC は RTC GPIO ではないので deep sleep では使えない)
// - BOOT(GPIO0) は RTC GPIO だが strapping pin でもある: deep sleep 起床は
//   チップリセットなので、起床のために押した BOOT の LOW がそのまま
//   ストラップサンプルに残り、UART download mode で起動してしまう
//   (= 復帰不能に見える)。そのため ext0 (BOOT) は deep sleep では張らない。
// - light sleep なら GPIO38 タッチや BOOT でも復帰できる

namespace board::sleep {

static const char* TAG = "board.sleep";

void arm_light_sleep_wake()
{
    // LOW アクティブ: タッチ INT / BOOT ボタン
    gpio_wakeup_enable(pins::kTouchInt, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable(pins::kButtonBoot, GPIO_INTR_LOW_LEVEL);
    // HIGH アクティブ: PWR ボタン
    gpio_wakeup_enable(pins::kButtonPwr, GPIO_INTR_HIGH_LEVEL);
    // LOW アクティブ: RTC INT (アラーム発火)。deep sleep では使えない
    // (RTC GPIO ではない) ので light sleep 専用。
    gpio_wakeup_enable(pins::kRtcInt, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    ESP_LOGI(TAG, "light sleep wake: touch(GPIO38,LOW) boot(GPIO0,LOW) pwr(GPIO10,HIGH) rtc(GPIO39,LOW)");
}

void arm_light_sleep_imu()
{
    // GPIO21 は imu_service が GPIO 割り込み (POSEDGE) で WoM を受けるので
    // gpio_wakeup_enable (LEVEL 型に int 型を書き換える) は呼べない。
    // light sleep が現状未使用のため何もしない。
    // TODO(hw): light sleep 実装時に再検討 (割り込み型の共存 or 排他)
}

void disarm_light_sleep_imu()
{
    gpio_wakeup_disable(pins::kImuInt1);
}

bool woke_by_imu()
{
    // esp_sleep_get_gpio_wakeup_status() は IDF v5.5 に無い。
    // WoM の INT1 は STATUS1 読取まで保持されるレベル出力（QMI8658C DS §6.1）
    // なので起床時にピンが HIGH なら IMU 起因とみなす。
    // TODO(hw): 実機で確認（短いパルスなら取りこぼす可能性あり）
    return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO &&
           gpio_get_level(pins::kImuInt1) == 1;
}

void enter_deep_sleep(uint64_t wake_after_us, bool imu_wake)
{
    // light sleep 用に貼り付けた歩数同期タイマー (imu_service が set) を
    // deep sleep に持ち込まない。以降は wake_after_us だけが timer になる。
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);

    // PWR(GPIO10) / IMU INT1(GPIO21) — ext1, どれかが HIGH で復帰
    // INT1 の極性は未確認 // TODO(hw): 実機で確認 (逆なら ANY_LOW/レベル変更)
    // imu_wake=false (raise_to_wake OFF) の時に INT1 を入れると、未クリアの
    // WoM ラッチや浮いたピンの HIGH で入眠直後に即起床するので外す。
    uint64_t ext1_mask = (1ULL << pins::kButtonPwr);
    if (imu_wake) {
        ext1_mask |= (1ULL << pins::kImuInt1);
    }
    esp_err_t ret =
        esp_sleep_enable_ext1_wakeup(ext1_mask, ESP_EXT1_WAKEUP_ANY_HIGH);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ext1 wake failed: %s", esp_err_to_name(ret));
    }

    if (wake_after_us > 0) {
        esp_sleep_enable_timer_wakeup(wake_after_us);
    }

    ESP_LOGI(TAG, "entering deep sleep (pwr ext1, imu=%s, timer=%s)",
             imu_wake ? "on" : "off", wake_after_us > 0 ? "on" : "off");
    esp_deep_sleep_start();
    // ここには戻らない
    for (;;) {
    }
}

}  // namespace board::sleep
