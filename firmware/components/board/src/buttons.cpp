#include "board/board.hpp"
#include "pins.hpp"

#include "esp_log.h"
#include "esp_check.h"
#include "iot_button.h"
#include "button_gpio.h"

namespace board::buttons {

static const char* TAG = "board.buttons";

// 長押し判定 800ms (plan.md G 章の初期割り当て)
static constexpr uint16_t kLongPressMs = 800;

static ButtonCallback s_cb = nullptr;

static void fire(Button b, ButtonEvent e)
{
    if (s_cb) {
        s_cb(b, e);
    }
}

static void cb_short(void*, void* usr)  { fire(static_cast<Button>(reinterpret_cast<intptr_t>(usr)), ButtonEvent::ShortPress); }
static void cb_long(void*, void* usr)   { fire(static_cast<Button>(reinterpret_cast<intptr_t>(usr)), ButtonEvent::LongPress); }
static void cb_double(void*, void* usr) { fire(static_cast<Button>(reinterpret_cast<intptr_t>(usr)), ButtonEvent::DoubleClick); }

static esp_err_t create_one(gpio_num_t gpio, uint8_t active_level, Button id, const char* name)
{
    const button_config_t cfg = {
        .long_press_time = kLongPressMs,
        .short_press_time = 0,  // 0 = ライブラリ既定
    };
    // スリープ復帰は board::sleep が GPIO wake でやるので enable_power_save は使わない
    const button_gpio_config_t gpio_cfg = {
        .gpio_num = gpio,
        .active_level = active_level,
        .enable_power_save = false,
        .disable_pull = false,  // TODO(hw): PWR=GPIO10 の外部プルダウン有無を実機で確認
    };
    button_handle_t handle = nullptr;
    esp_err_t ret = iot_button_new_gpio_device(&cfg, &gpio_cfg, &handle);
    if (ret != ESP_OK || handle == nullptr) {
        return ret != ESP_OK ? ret : ESP_FAIL;
    }

    void* usr = reinterpret_cast<void*>(static_cast<intptr_t>(id));
    ESP_ERROR_CHECK_WITHOUT_ABORT(iot_button_register_cb(handle, BUTTON_SINGLE_CLICK, nullptr, cb_short, usr));
    ESP_ERROR_CHECK_WITHOUT_ABORT(iot_button_register_cb(handle, BUTTON_LONG_PRESS_START, nullptr, cb_long, usr));
    ESP_ERROR_CHECK_WITHOUT_ABORT(iot_button_register_cb(handle, BUTTON_DOUBLE_CLICK, nullptr, cb_double, usr));

    ESP_LOGI(TAG, "%s: GPIO%d active=%s", name, (int)gpio, active_level ? "HIGH" : "LOW");
    return ESP_OK;
}

esp_err_t init()
{
    esp_err_t r1 = create_one(pins::kButtonBoot, 0, Button::Boot, "BOOT");
    esp_err_t r2 = create_one(pins::kButtonPwr, 1, Button::Pwr, "PWR");
    return (r1 == ESP_OK && r2 == ESP_OK) ? ESP_OK : ESP_FAIL;
}

void set_callback(ButtonCallback cb)
{
    s_cb = cb;
}

}  // namespace board::buttons
