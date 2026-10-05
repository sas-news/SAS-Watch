#include "board/board.hpp"
#include "pins.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"

namespace board::haptics {

static const char* TAG = "board.haptics";

// GPIO18 -> NPN でモーター駆動。モーター本体の同梱は未確認 // TODO(hw): 実機で確認
static esp_timer_handle_t s_off_timer = nullptr;
static bool s_ready = false;

static void IRAM_ATTR off_cb(void*)
{
    gpio_set_level(pins::kMotor, 0);
}

esp_err_t init()
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pins::kMotor,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    const esp_timer_create_args_t args = {
        .callback = &off_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "haptics_off",
        .skip_unhandled_events = false,
    };
    ret = esp_timer_create(&args, &s_off_timer);
    if (ret != ESP_OK) {
        return ret;
    }

    s_ready = true;
    ESP_LOGI(TAG, "ok (GPIO%d)", (int)pins::kMotor);
    return ESP_OK;
}

void pulse(uint32_t ms)
{
    if (!s_ready || ms == 0) {
        return;
    }
    gpio_set_level(pins::kMotor, 1);
    esp_timer_stop(s_off_timer);  // 連続パルスの延長用
    esp_timer_start_once(s_off_timer, (uint64_t)ms * 1000);
}

}  // namespace board::haptics
