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

}  // namespace ui::port
