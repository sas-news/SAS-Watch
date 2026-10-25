#include "board/board.hpp"
#include "pins.hpp"

#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "iot_button.h"
#include "button_gpio.h"

namespace board::buttons {

static const char* TAG = "board.buttons";

// 長押し判定 800ms (plan.md G 章の初期割り当て)
static constexpr uint16_t kLongPressMs = 800;

static ButtonCallback s_cb = nullptr;
static bool (*s_dbl_pred)(Button) = nullptr;
// 押下中に LONG_PRESS_START が出たか (PRESS_UP での即時発火を抑止する)。
// ボタンタスクからのみ読み書きされる。
static bool s_long_fired[2] = {};

static size_t idx_of(Button b) { return static_cast<size_t>(b); }
static Button btn_of(void* usr) {
    return static_cast<Button>(reinterpret_cast<intptr_t>(usr));
}

// 実機の「PWR を押しても反応しない」を切り分けるためのログ。押下は稀なので
// 毎回出す (down/up で生の極性も残す — 外部プルや配線の確認用)。
static const char* name_of(Button b)
{
    return b == Button::Boot ? "BOOT" : "PWR";
}
static gpio_num_t pin_of(Button b)
{
    return b == Button::Boot ? pins::kButtonBoot : pins::kButtonPwr;
}
static void log_event(Button b, const char* what)
{
    ESP_LOGI(TAG, "%s %s level=%d", name_of(b), what,
             static_cast<int>(gpio_get_level(pin_of(b))));
}

static void fire(Button b, ButtonEvent e)
{
    const char* ev = e == ButtonEvent::ShortPress ? "short"
                     : e == ButtonEvent::LongPress ? "long"
                                                   : "double";
    log_event(b, ev);
    if (s_cb) {
        s_cb(b, e);
    }
}

// 2回押しが有効なボタンか。predicate 未登録なら全ボタン有効 (=従来動作)。
static bool double_enabled(Button b)
{
    return s_dbl_pred == nullptr || s_dbl_pred(b);
}

static void cb_press_down(void*, void* usr)
{
    const Button b = btn_of(usr);
    s_long_fired[idx_of(b)] = false;
    log_event(b, "down");
}
static void cb_press_up(void*, void* usr)
{
    const Button b = btn_of(usr);
    log_event(b, "up");
    // 長押し済みでなく、2回押しが無効なボタンは即 ShortPress。
    // 2回押し有効なボタンは従来どおり SINGLE_CLICK/DOUBLE_CLICK を待つ。
    if (!s_long_fired[idx_of(b)] && !double_enabled(b)) {
        fire(b, ButtonEvent::ShortPress);
    }
}
static void cb_long(void*, void* usr)
{
    const Button b = btn_of(usr);
    s_long_fired[idx_of(b)] = true;
    fire(b, ButtonEvent::LongPress);
}
static void cb_short(void*, void* usr)
{
    const Button b = btn_of(usr);
    // 2回押し有効なボタンのみここで発火 (PRESS_UP では出していない)。
    if (double_enabled(b)) fire(b, ButtonEvent::ShortPress);
}
static void cb_double(void*, void* usr)
{
    const Button b = btn_of(usr);
    // 2回押し無効なら各 PRESS_UP で既に発火しているので二重発火させない。
    if (double_enabled(b)) fire(b, ButtonEvent::DoubleClick);
}

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
    ESP_ERROR_CHECK_WITHOUT_ABORT(iot_button_register_cb(handle, BUTTON_PRESS_DOWN, nullptr, cb_press_down, usr));
    ESP_ERROR_CHECK_WITHOUT_ABORT(iot_button_register_cb(handle, BUTTON_PRESS_UP, nullptr, cb_press_up, usr));
    ESP_ERROR_CHECK_WITHOUT_ABORT(iot_button_register_cb(handle, BUTTON_LONG_PRESS_START, nullptr, cb_long, usr));
    ESP_ERROR_CHECK_WITHOUT_ABORT(iot_button_register_cb(handle, BUTTON_SINGLE_CLICK, nullptr, cb_short, usr));
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

void set_double_click_predicate(bool (*fn)(Button))
{
    s_dbl_pred = fn;
}

}  // namespace board::buttons
