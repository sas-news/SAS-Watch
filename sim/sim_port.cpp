// sim_port.cpp — ui::port の Linux スタブ実装。
// シングルスレッドなので lock/crit は no-op。時計は sim の SimClock。
#include "sim_platform.hpp"
#include "ui/port.hpp"
#include "lvgl.h"
#include "board/power_consts.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace ui::port {

bool lock(uint32_t) { return true; }
void unlock() {}

void crit_enter() {}
void crit_exit() {}

int64_t now_ms() { return sim::clock().now_ms(); }

// スクリーンショット用の固定値。
int battery_percent() { return 87; }
bool battery_charging() { return false; }

void vibrate(uint32_t ms) { std::printf("[sim] vibrate %ums\n", ms); }

[[noreturn]] void restart() {
  std::printf("[sim] restart requested\n");
  std::exit(0);
}

size_t device_info(char* buf, size_t cap) {
  const int n = std::snprintf(buf, cap, "sim / SAS-Watch dev / LVGL %d.%d",
                            LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR);
  return n > 0 ? static_cast<size_t>(n) : 0;
}

int power_off_hold_seconds() { return board::kPowerOffHoldSeconds; }

void display_power(bool on) {
  std::printf("[sim] display_power %s\n", on ? "on" : "off");
}

void brightness_apply(int percent) {
  std::printf("[sim] brightness %d%%\n", percent);
}

// ---- テーマ資産 (ホスト fs の "sim/themes" + malloc アリーナ) ----

constexpr uint32_t kThemeArenaSize = 3 * 1024 * 1024;
uint8_t* s_arena = nullptr;
uint32_t s_arena_used = 0;

const char* theme_assets_root() { return "sim/themes"; }

void theme_assets_reset() {
  if (!s_arena) s_arena = static_cast<uint8_t*>(std::malloc(kThemeArenaSize));
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
  s_arena_used += (static_cast<uint32_t>(sz) + 7u) & ~7u;
  *out = dst;
  *out_len = static_cast<uint32_t>(sz);
  return true;
}

}  // namespace ui::port
