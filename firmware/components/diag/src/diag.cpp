#include "diag/diag.hpp"

#include <cinttypes>

#include "esp_log.h"
#include "esp_idf_version.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "esp_rom_sys.h"
#include "driver/i2c_master.h"
#include "bsp/esp-bsp.h"

namespace diag {

static const char* TAG = "diag";

void print_system_info()
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    esp_flash_get_size(nullptr, &flash_size);

    ESP_LOGI(TAG, "chip=%s cores=%d rev=%d.%d",
             CONFIG_IDF_TARGET, chip.cores,
             (int)chip.revision / 100, (int)chip.revision % 100);
    ESP_LOGI(TAG, "flash=%" PRIu32 "MB %s",
             flash_size / (1024 * 1024),
             (chip.features & CHIP_FEATURE_EMB_FLASH) ? "(embedded)" : "(external)");
    ESP_LOGI(TAG, "psram=%" PRIu32 "KB free, internal heap=%" PRIu32 "KB free (min %" PRIu32 "KB)",
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024,
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
             heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024);
    ESP_LOGI(TAG, "reset_reason=%d idf=%s",
             (int)esp_reset_reason(), esp_get_idf_version());
    // TODO(hw): 実機で GD25Q256 (32MB) が見えるか確認 -> sdkconfig の FLASHSIZE を見直す
}

void i2c_scan()
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == nullptr) {
        ESP_LOGW(TAG, "i2c bus not initialized");
        return;
    }

    // 期待値 (docs/board.md): 0x34 PMIC / 0x38 タッチ / 0x51 RTC / 0x6B IMU /
    // 0x40 ES7210 / 0x18 ES8311
    ESP_LOGI(TAG, "i2c scan start (expect 0x18,0x34,0x38,0x40,0x51,0x6B)");
    int found = 0;
    for (uint16_t addr = 0x03; addr < 0x78; addr++) {
        if (i2c_master_probe(bus, addr, 50) == ESP_OK) {
            ESP_LOGI(TAG, "  found: 0x%02X", addr);
            found++;
        }
    }
    ESP_LOGI(TAG, "i2c scan done: %d device(s)", found);
    // TODO(hw): 実機で全デバイスが応答するか確認
}

}  // namespace diag
