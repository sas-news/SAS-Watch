#include "board/board.hpp"
#include "pins.hpp"

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_check.h"
#include "driver/gpio.h"

// docs/board.md / plan.md A-3:
// - deep sleep から復帰できるのは BOOT(GPIO0), PWR(GPIO10), IMU(GPIO21), timer だけ
//   (GPIO38 タッチ, GPIO39 RTC は RTC GPIO ではないので deep sleep では使えない)
// - light sleep なら GPIO38 タッチでも復帰できる

namespace board::sleep {

static const char* TAG = "board.sleep";

void arm_light_sleep_wake()
{
    // LOW アクティブ: タッチ INT / BOOT ボタン
    gpio_wakeup_enable(pins::kTouchInt, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable(pins::kButtonBoot, GPIO_INTR_LOW_LEVEL);
    // HIGH アクティブ: PWR ボタン
    gpio_wakeup_enable(pins::kButtonPwr, GPIO_INTR_HIGH_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    ESP_LOGI(TAG, "light sleep wake: touch(GPIO38,LOW) boot(GPIO0,LOW) pwr(GPIO10,HIGH)");
}

void arm_light_sleep_imu()
{
    // INT1(GPIO21) は HIGH アクティブ想定。
    // TODO(hw): 実機で極性を確認 (逆なら GPIO_INTR_LOW_LEVEL)
    gpio_wakeup_enable(pins::kImuInt1, GPIO_INTR_HIGH_LEVEL);
    esp_sleep_enable_gpio_wakeup();
}

void disarm_light_sleep_imu()
{
    gpio_wakeup_disable(pins::kImuInt1);
}

bool woke_by_imu()
{
    return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO &&
           (esp_sleep_get_gpio_wakeup_status() & (1ULL << pins::kImuInt1)) != 0;
}

void enter_deep_sleep(uint64_t wake_after_us)
{
    // BOOT(GPIO0, LOW) — ext0 (単一 RTC GPIO)
    esp_err_t ret = esp_sleep_enable_ext0_wakeup(pins::kButtonBoot, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ext0 wake failed: %s", esp_err_to_name(ret));
    }

    // PWR(GPIO10) / IMU INT1(GPIO21) — ext1, どれかが HIGH で復帰
    // INT1 の極性は未確認 // TODO(hw): 実機で確認 (逆なら ANY_LOW/レベル変更)
    const uint64_t ext1_mask = (1ULL << pins::kButtonPwr) | (1ULL << pins::kImuInt1);
    ret = esp_sleep_enable_ext1_wakeup(ext1_mask, ESP_EXT1_WAKEUP_ANY_HIGH);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ext1 wake failed: %s", esp_err_to_name(ret));
    }

    if (wake_after_us > 0) {
        esp_sleep_enable_timer_wakeup(wake_after_us);
    }

    ESP_LOGI(TAG, "entering deep sleep (timer wake %s)",
             wake_after_us > 0 ? "on" : "off");
    esp_deep_sleep_start();
    // ここには戻らない
    for (;;) {
    }
}

}  // namespace board::sleep
