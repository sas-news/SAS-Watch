// esp_port.cpp — ui::port の ESP-IDF 実装。
// LVGL ロック = lvgl_port_lock、クリティカル = portMUX、
// 電池 = board::pmic、再起動 = esp_restart。
#include "ui/port.hpp"

#include "audio/audio.hpp"
#include "board/board.hpp"
#include "board/power_consts.h"
#include "esp_heap_caps.h"
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

void click() { audio::click(); }

[[noreturn]] void restart() { esp_restart(); }

size_t device_info(char* buf, size_t cap) {
  esp_chip_info_t ci;
  esp_chip_info(&ci);
  const int n = std::snprintf(
      buf, cap, "ESP32-S3 rev%d / %d core", static_cast<int>(ci.revision),
      static_cast<int>(ci.cores));
  return n > 0 ? static_cast<size_t>(n) : 0;
}

int power_off_hold_seconds() { return board::kPowerOffHoldSeconds; }

// ---- テーマ資産 (littlefs "/assets/themes" + PSRAM アリーナ) ----

// TODO(hw): 実機で確認 — PSRAM 8MB から 3MB をテーマ作業領域に割く。
// zip 受信展開と画像アリーナで共用 (同時に使わないので問題ない)。
constexpr uint32_t kThemeArenaSize = 3 * 1024 * 1024;
uint8_t* s_arena = nullptr;
uint32_t s_arena_used = 0;

const char* theme_assets_root() { return "/assets/themes"; }

void theme_assets_reset() {
  if (!s_arena) {
    // 動的確保は初期化時のみ (ui::create → theme_apply → ここ)。
    s_arena = static_cast<uint8_t*>(
        heap_caps_malloc(kThemeArenaSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  s_arena_used = 0;
}

bool theme_asset_load(const char* path, const uint8_t** out,
                      uint32_t* out_len) {
  if (!s_arena || !out || !out_len) return false;
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long sz = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (sz < 0 || static_cast<uint64_t>(s_arena_used) + sz + 8 > kThemeArenaSize) {
    std::fclose(f);
    return false;
  }
  uint8_t* dst = s_arena + s_arena_used;
  const size_t got = std::fread(dst, 1, static_cast<size_t>(sz), f);
  std::fclose(f);
  if (got != static_cast<size_t>(sz)) return false;
  // 8B アラインで次の確保位置を進める。
  s_arena_used += (static_cast<uint32_t>(sz) + 7u) & ~7u;
  *out = dst;
  *out_len = static_cast<uint32_t>(sz);
  return true;
}

}  // namespace ui::port
