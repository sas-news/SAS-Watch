// esp_port.cpp — ui::port の ESP-IDF 実装。
// LVGL ロック = lvgl_port_lock、クリティカル = portMUX、
// 電池 = board::pmic、再起動 = esp_restart。
#include "ui/port.hpp"

#include "board/board.hpp"
#include "esp_idf_version.h"
#include "esp_lvgl_port.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_chip_info.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "sdkconfig.h"

#include <cstdio>
#include <cstring>

namespace {

portMUX_TYPE s_crit = portMUX_INITIALIZER_UNLOCKED;

}  // namespace

namespace ui::port {

bool lock(uint32_t timeout_ms) { return lvgl_port_lock(timeout_ms); }
void unlock() { lvgl_port_unlock(); }

void crit_enter() { portENTER_CRITICAL_SAFE(&s_crit); }
void crit_exit() { portEXIT_CRITICAL_SAFE(&s_crit); }

int64_t now_ms() { return esp_timer_get_time() / 1000; }

int battery_percent() { return board::pmic::battery_percent(); }
bool battery_charging() { return board::pmic::is_charging(); }

void vibrate(uint32_t ms) { board::haptics::pulse(ms); }

[[noreturn]] void restart() { esp_restart(); }

size_t device_info(char* buf, size_t cap) {
  esp_chip_info_t ci;
  esp_chip_info(&ci);
  const int n = std::snprintf(
      buf, cap, "ESP32-S3 rev%d / %d core", static_cast<int>(ci.revision),
      static_cast<int>(ci.cores));
  return n > 0 ? static_cast<size_t>(n) : 0;
}

}  // namespace ui::port
