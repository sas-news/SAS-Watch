// sim_port.cpp — ui::port の Linux スタブ実装。
// シングルスレッドなので lock/crit は no-op。時計は sim の SimClock。
#include "sim_platform.hpp"
#include "ui/port.hpp"
#include "lvgl.h"
#include "board/power_consts.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "watch/theme/package.hpp"
#include "watch/theme/zipfile.hpp"

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

void click() {}  // sim は無音

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

// ---- ファーム更新 (スナップショット用の固定値、sim::set_ota_debug で差替) ----

const char* fw_version() { return "0.1.0"; }

OtaView s_ota;  // NOLINT - sim_platform 経由で設定される

void ota_status(OtaView* out) { if (out) *out = s_ota; }

void display_power(bool on) {
  std::printf("[sim] display_power %s\n", on ? "on" : "off");
}

void brightness_apply(int percent) {
  std::printf("[sim] brightness %d%%\n", percent);
}

// ---- テーマ資産 (ホスト fs の "sim/themes" + malloc アリーナ) ----
// 構成は esp_port.cpp と同じ (zip 優先・旧 dir フォールバック)。

constexpr uint32_t kScreenBlocks = 2;
constexpr uint32_t kGlobalOff = kScreenBlocks * kThemeScreenBlockSize;
constexpr uint32_t kThemeArenaSize = 4 * 1024 * 1024;
uint8_t* s_arena = nullptr;
uint32_t s_arena_used = kGlobalOff;
bool s_blk_used[kScreenBlocks] = {};

const char* theme_assets_root() { return "sim/themes"; }

void theme_assets_reset() {
  if (!s_arena) s_arena = static_cast<uint8_t*>(std::malloc(kThemeArenaSize));
  s_arena_used = kGlobalOff;
  for (auto& u : s_blk_used) u = false;
}

namespace {

struct ZipCtx {
  FILE* f;
};

bool zip_at(void* user, uint32_t off, uint8_t* dst, uint32_t n) {
  FILE* f = static_cast<ZipCtx*>(user)->f;
  return std::fseek(f, static_cast<long>(off), SEEK_SET) == 0 &&
         std::fread(dst, 1, n, f) == n;
}

int zip_open_list(const char* id, FILE** out_f,
                  watch::ThemePackageEntry* list, int cap) {
  char path[128];
  std::snprintf(path, sizeof(path), "%s/%s.zip", theme_assets_root(), id);
  FILE* f = std::fopen(path, "rb");
  if (!f) return -1;
  std::fseek(f, 0, SEEK_END);
  const long fsz = std::ftell(f);
  if (fsz <= 0) {
    std::fclose(f);
    return -1;
  }
  ZipCtx zc{f};
  watch::ThemeZipSrc z{&zc, &zip_at, static_cast<uint32_t>(fsz)};
  static uint8_t tail[4096];
  const int cnt = watch::theme_zipfile_list(z, list, cap, tail, sizeof(tail));
  if (cnt < 0) {
    std::fclose(f);
    return cnt;
  }
  *out_f = f;
  return cnt;
}

bool zip_entry_off(FILE* f, const watch::ThemePackageEntry& e,
                   uint32_t* off) {
  ZipCtx zc{f};
  std::fseek(f, 0, SEEK_END);
  watch::ThemeZipSrc z{&zc, &zip_at,
                       static_cast<uint32_t>(std::ftell(f))};
  return watch::theme_zipfile_data_offset(z, e, off);
}

}  // namespace

bool theme_entry_stat(const char* id, const char* name, uint32_t* out_size) {
  if (!id || !name || !out_size) return false;
  {
    static watch::ThemePackageEntry list[watch::kThemePackageMaxEntries];
    FILE* f = nullptr;
    const int cnt = zip_open_list(id, &f, list,
                                  watch::kThemePackageMaxEntries);
    if (cnt >= 0) {
      const watch::ThemePackageEntry* e =
          watch::theme_package_find(list, cnt, name);
      std::fclose(f);
      if (e) {
        *out_size = e->size;
        return true;
      }
    }
  }
  char path[160];
  std::snprintf(path, sizeof(path), "%s/%s/%s", theme_assets_root(), id,
                name);
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long sz = std::ftell(f);
  std::fclose(f);
  if (sz < 0) return false;
  *out_size = static_cast<uint32_t>(sz);
  return true;
}

bool theme_entry_read(const char* id, const char* name, uint8_t* dst,
                      uint32_t cap) {
  if (!id || !name || !dst) return false;
  {
    static watch::ThemePackageEntry list[watch::kThemePackageMaxEntries];
    FILE* f = nullptr;
    const int cnt = zip_open_list(id, &f, list,
                                  watch::kThemePackageMaxEntries);
    if (cnt >= 0) {
      const watch::ThemePackageEntry* e =
          watch::theme_package_find(list, cnt, name);
      bool ok = false;
      if (e && e->size <= cap) {
        uint32_t off = 0;
        ok = zip_entry_off(f, *e, &off) &&
             std::fseek(f, static_cast<long>(off), SEEK_SET) == 0 &&
             std::fread(dst, 1, e->size, f) == e->size;
      }
      std::fclose(f);
      if (ok) return true;
      if (!e) goto dir_fallback;
      return false;
    }
  }
dir_fallback: {
  char path[160];
  std::snprintf(path, sizeof(path), "%s/%s/%s", theme_assets_root(), id,
                name);
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long sz = std::ftell(f);
  if (sz < 0 || static_cast<uint32_t>(sz) > cap) {
    std::fclose(f);
    return false;
  }
  std::fseek(f, 0, SEEK_SET);
  const bool ok = std::fread(dst, 1, static_cast<size_t>(sz), f) ==
                  static_cast<size_t>(sz);
  std::fclose(f);
  return ok;
}
}

bool theme_asset_load(const char* id, const char* name, const uint8_t** out,
                      uint32_t* out_len) {
  if (!s_arena || !out || !out_len) return false;
  uint32_t sz = 0;
  if (!theme_entry_stat(id, name, &sz) || sz == 0) return false;
  if (static_cast<uint64_t>(s_arena_used) + sz + 8 > kThemeArenaSize) {
    return false;
  }
  uint8_t* dst = s_arena + s_arena_used;
  if (!theme_entry_read(id, name, dst, sz)) return false;
  s_arena_used += (sz + 7u) & ~7u;
  *out = dst;
  *out_len = sz;
  return true;
}

uint8_t* theme_asset_alloc(uint32_t n) {
  if (!s_arena || static_cast<uint64_t>(s_arena_used) + n + 8 >
                      kThemeArenaSize) {
    return nullptr;
  }
  uint8_t* p = s_arena + s_arena_used;
  s_arena_used += (n + 7u) & ~7u;
  return p;
}

uint8_t* theme_screen_block() {
  if (!s_arena) return nullptr;
  for (uint32_t i = 0; i < kScreenBlocks; ++i) {
    if (!s_blk_used[i]) {
      s_blk_used[i] = true;
      return s_arena + i * kThemeScreenBlockSize;
    }
  }
  return nullptr;
}

void theme_screen_free(uint8_t* p) {
  if (!s_arena || !p) return;
  for (uint32_t i = 0; i < kScreenBlocks; ++i) {
    if (s_arena + i * kThemeScreenBlockSize == p) {
      s_blk_used[i] = false;
      return;
    }
  }
}

uint8_t* theme_tmp_alloc(uint32_t n) {
  return static_cast<uint8_t*>(std::malloc(n));
}
void theme_tmp_free(uint8_t* p) { std::free(p); }

}  // namespace ui::port

namespace sim {

void set_ota_debug(int stage, int pct, const char* msg, const char* version) {
  ui::port::s_ota.stage = stage;
  ui::port::s_ota.pct = pct;
  std::snprintf(ui::port::s_ota.msg, sizeof(ui::port::s_ota.msg), "%s",
                msg ? msg : "");
  std::snprintf(ui::port::s_ota.version, sizeof(ui::port::s_ota.version), "%s",
                version ? version : "");
}

}  // namespace sim
