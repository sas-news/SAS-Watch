// SAS-Watch firmware: Phase 2 の最小形 (docs/plan.md R 章)
// 起動 -> 診断ログ -> board 初期化 -> 時計画面 -> ボタン/タッチで復帰の簡易電源管理
#include <stdio.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_err.h"

#include "board/board.hpp"
#include "diag/diag.hpp"
#include "ui_min/ui_min.hpp"

static const char* TAG = "app";

// 簡易電源管理 (本格的な PowerManager は core と統合するときに作る: plan.md L 章)
static constexpr int64_t kScreenOffAfterUs = 10LL * 1000 * 1000;  // 無操作 10 秒で画面オフ
static int64_t s_last_activity_us = 0;

static void kick_activity()
{
    s_last_activity_us = esp_timer_get_time();
    if (!ui_min::is_screen_on()) {
        ui_min::set_screen_on(true);
    }
}

static void on_button(board::Button b, board::ButtonEvent e)
{
    const char* bname = (b == board::Button::Boot) ? "BOOT" : "PWR";
    const char* ename =
        e == board::ButtonEvent::ShortPress ? "short" :
        e == board::ButtonEvent::LongPress  ? "long" : "double";
    ESP_LOGI(TAG, "button %s %s", bname, ename);

    char msg[32];
    snprintf(msg, sizeof(msg), "%s %s", bname, ename);
    ui_min::notify(msg);
    kick_activity();
}

static void idle_check(void*)
{
    if (ui_min::is_screen_on() &&
        esp_timer_get_time() - s_last_activity_us > kScreenOffAfterUs) {
        ESP_LOGI(TAG, "idle %llds -> screen off", kScreenOffAfterUs / 1000000);
        ui_min::set_screen_on(false);
    }
}

extern "C" void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 表示用タイムゾーン (BLE の time.set で同期するまでの暫定値。RTC 本体は UTC epoch)
    setenv("TZ", "JST-9", 1);
    tzset();

    ESP_LOGI(TAG, "=== SAS-Watch firmware boot ===");
    diag::print_system_info();

    ESP_ERROR_CHECK(board::init());
    diag::i2c_scan();

    if (ui_min::init() == nullptr) {
        ESP_LOGE(TAG, "ui_min init failed");
    }
    ui_min::set_battery(board::pmic::battery_percent(), board::pmic::is_charging());

    board::buttons::set_callback(on_button);
    ui_min::set_on_activity(kick_activity);
    kick_activity();

    const esp_timer_create_args_t idle_args = {
        .callback = &idle_check,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "idle_check",
        .skip_unhandled_events = false,
    };
    esp_timer_handle_t idle_timer = nullptr;
    ESP_ERROR_CHECK(esp_timer_create(&idle_args, &idle_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(idle_timer, 1000 * 1000));

    // AXP IRQ 線は無いので電池状態は定期ポーリング (plan.md A-3)
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ui_min::set_battery(board::pmic::battery_percent(), board::pmic::is_charging());
    }
}
