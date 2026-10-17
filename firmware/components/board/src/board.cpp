#include "board/board.hpp"

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "bsp/esp-bsp.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"

namespace board {

static const char* TAG = "board";

// 前回の転送中断 (リセット/深眠跨ぎ) でスレーブが SDA を LOW に保持したままだと、
// 以降の全 I2C transaction が timeout になる (実機で i2c scan 0 devices を観測)。
// バス作成前に SCL を GPIO で 9 パルス送ってスレーブの状態機械を開放する定番処理。
static void i2c_bus_recover()
{
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << BSP_I2C_SCL) | (1ULL << BSP_I2C_SDA);
    io.mode = GPIO_MODE_INPUT_OUTPUT_OD;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io);
    gpio_set_level(BSP_I2C_SDA, 1);
    gpio_set_level(BSP_I2C_SCL, 1);
    esp_rom_delay_us(10);
    for (int i = 0; i < 9 && gpio_get_level(BSP_I2C_SDA) == 0; ++i) {
        gpio_set_level(BSP_I2C_SCL, 0);
        esp_rom_delay_us(10);
        gpio_set_level(BSP_I2C_SCL, 1);
        esp_rom_delay_us(10);
    }
    const int sda = gpio_get_level(BSP_I2C_SDA);
    if (sda == 0) {
        ESP_LOGW(TAG, "i2c bus recovery failed: SDA still low");
    }
    gpio_reset_pin(BSP_I2C_SCL);
    gpio_reset_pin(BSP_I2C_SDA);
}

esp_err_t init()
{
    i2c_bus_recover();
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
