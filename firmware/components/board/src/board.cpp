#include "board/board.hpp"

#include "esp_log.h"
#include "bsp/esp-bsp.h"

namespace board {

static const char* TAG = "board";

esp_err_t init()
{
    // 全デバイス共通の I2C バス (GPIO14=SCL, GPIO15=SDA) は BSP が持つ
    esp_err_t ret = bsp_i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bsp_i2c_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 個別デバイスは失敗しても続行 (実機なし前提・I2Cスキャンを diag で出す)
    const struct {
        const char* name;
        esp_err_t (*fn)(void);
    } steps[] = {
        {"pmic", pmic::init},
        {"rtc", rtc::init},
        {"imu", imu::init},
        {"buttons", buttons::init},
        {"haptics", haptics::init},
    };
    for (const auto& s : steps) {
        const esp_err_t r = s.fn();
        if (r == ESP_OK) {
            ESP_LOGI(TAG, "%s: ok", s.name);
        } else {
            ESP_LOGW(TAG, "%s: init failed (%s) // TODO(hw): 実機で確認", s.name, esp_err_to_name(r));
        }
    }
    return ESP_OK;
}

}  // namespace board
