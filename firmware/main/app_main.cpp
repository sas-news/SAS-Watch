// SAS-Watch firmware: Phase 4-6 MVP (docs/plan.md R 章)
// 起動 -> 診断 -> board -> 表示/LVGL -> watch_app (core Runtime タスク) -> UI 組立
// BLE は別セッションの ble_link コンポーネント。未導入でもビルドできるよう
// weak スタブ + CONFIG_SAS_BLE_LINK ガードで呼び出す。
#include <stdio.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

#include "board/board.hpp"
#include "diag/diag.hpp"
#include "ota/ota.hpp"
#include "platform_esp/platform_esp.hpp"
#include "theme_store/theme_store.hpp"
#include "ui/face_data.hpp"
#include "ui/ui.hpp"
#include "watch_app/watch_app.hpp"
#include "watch/event.hpp"
#include "watch/features/alarm.hpp"
#include "watch/features/notify.hpp"
#include "watch/features/steps.hpp"

static const char* TAG = "app";

// platform 実体は静的確保。
static platform_esp::EspClock s_clock;
static platform_esp::NvsKv s_kv;

#if CONFIG_SAS_BLE_LINK
#include "ble_link.h"
#endif

// 電池ポーリング (AXP IRQ 線が無いので定期 — plan.md A-3)。
// 変化があったときだけ Event を出す。
static void battery_poll(void*) {
  int last_pct = -2;
  bool last_chg = false;
  while (true) {
    const int pct = board::pmic::battery_percent();
    const bool chg = board::pmic::is_charging();
    if (pct != last_pct) {
      last_pct = pct;
      watch_app::bus().publish(
          {watch::EventType::BatteryChanged,
           static_cast<uint32_t>(pct < 0 ? 0 : pct)});
    }
    if (chg != last_chg) {
      last_chg = chg;
      watch_app::bus().publish(
          {watch::EventType::ChargingChanged, chg ? 1u : 0u});
    }
    vTaskDelay(pdMS_TO_TICKS(30000));
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

    platform_esp::install_log();
    ESP_LOGI(TAG, "=== SAS-Watch firmware boot ===");
    diag::print_system_info();

    ESP_ERROR_CHECK(board::init());
    diag::i2c_scan();

    if (!s_kv.init()) {
        // NVS が読めなくても既定値で動く。エラーは init 内でログ済み。
        ESP_LOGW(TAG, "kv init failed; running with defaults");
    }

    // テーマ資産用 littlefs (/assets)。失敗しても内蔵テーマだけで動く。
    if (!theme_store::init()) {
        ESP_LOGW(TAG, "theme assets unavailable; builtin themes only");
    }

    // 表示・LVGLタスク・タッチを起こす。
    if (ui::init_display() == nullptr) {
        ESP_LOGE(TAG, "display init failed");
        return;  // 表示なしでは時計として成立しないので止める
    }

    // core Runtime タスク (キュー・電源・Feature)。
    if (!watch_app::start({&s_clock, &s_kv})) {
        ESP_LOGE(TAG, "watch_app start failed");
        return;
    }

    // 文字盤の補助データ (ui/face_data.hpp)。歩数は steps Feature と
    // settings.steps_goal から。通知数は notify Feature、次のアラームは
    // alarm_next_fire_epoch() を当日のローカル min-of-day に変換。
    static const ui::face_data::Hooks kFaceData = {
        []() -> int32_t {
#if SAS_APP_STEPS
            return static_cast<int32_t>(watch::features::steps_today());
#else
            return -1;
#endif
        },
        []() -> int32_t {
            return static_cast<int32_t>(watch_app::settings().steps_goal);
        },
        []() -> int32_t {
#if SAS_APP_NOTIFY
            return static_cast<int32_t>(watch::features::notify_count());
#else
            return -1;
#endif
        },
        []() -> int32_t {
#if SAS_APP_ALARM
            const int64_t e = watch::features::alarm_next_fire_epoch();
            if (e <= 0) return -1;
            const int64_t local =
                e + watch_app::settings().tz_offset_min * 60;
            return static_cast<int32_t>(((local % 86400) + 86400) % 86400 /
                                        60);
#else
            return -1;
#endif
        },
    };
    ui::face_data::set_hooks(&kFaceData);

    // UI 組み立て (LVGL を触るのでロック内)。
    if (lvgl_port_lock(2000)) {
        ui::create({&watch_app::bus(), &watch_app::navigator(),
                    &watch_app::settings(), watch_app::fctx()});
        lvgl_port_unlock();
    } else {
        ESP_LOGE(TAG, "lvgl lock timeout; UI not created");
    }

#if CONFIG_SAS_BLE_LINK
    // パスキー確認の「はい/いいえ」→ NimBLE へ
    ui::set_passkey_confirm([](bool ok) { ble_link_confirm_passkey(ok); });
    ble_link_start(watch_app::ble_config());
#endif

    xTaskCreate(battery_poll, "batt", 2048, nullptr, 3, nullptr);

    // OTA 直後の初回起動なら、ここまで初期化が全部通った = 自己診断 OK として
    // 新イメージを mark valid にする (次回リセットで旧イメージに戻されない)。
    ota::mark_valid_if_pending();
}
