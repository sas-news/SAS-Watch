// esp_port.cpp — ui::port の ESP-IDF 実装。
// LVGL ロック = lvgl_port_lock、クリティカル = portMUX、
// 電池 = board::pmic、再起動 = esp_restart。
#include "ui/port.hpp"

#include "audio/audio.hpp"
#include "board/board.hpp"
#include "board/power_consts.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_app_desc.h"
#include "esp_lvgl_port.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_chip_info.h"
#include "ota/ota.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "sdkconfig.h"

#include <cstdio>
#include <cstring>

#include "watch/theme/package.hpp"
#include "watch/theme/zipfile.hpp"

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

// ---- ファーム更新 ----

const char* fw_version() { return esp_app_get_description()->version; }

void ota_status(OtaView* out) {
  if (!out) return;
  const ota::Status s = ota::status();
  out->stage = static_cast<int>(s.stage);
  out->pct = s.pct;
  std::strncpy(out->msg, s.msg, sizeof(out->msg) - 1);
  out->msg[sizeof(out->msg) - 1] = '\0';
  std::strncpy(out->version, s.version, sizeof(out->version) - 1);
  out->version[sizeof(out->version) - 1] = '\0';
}

// ---- テーマ資産 (littlefs "/assets/themes" + PSRAM アリーナ) ----

// TODO(hw): 実機で確認 — PSRAM 8MB から 4MB をテーマ作業領域に割く。
// 先頭は画面背景用ブロック 640KiB×2 (現画面+Home が同時に居る)、
// 残り ~2.7MiB をグローバル資産 (テーマ適用単位で使い切り) に使う。
constexpr uint32_t kScreenBlocks = 2;
constexpr uint32_t kGlobalOff = kScreenBlocks * kThemeScreenBlockSize;
constexpr uint32_t kThemeArenaSize = 4 * 1024 * 1024;
uint8_t* s_arena = nullptr;
uint32_t s_arena_used = kGlobalOff;
bool s_blk_used[kScreenBlocks] = {};

const char* theme_assets_root() { return "/assets/themes"; }

void theme_assets_reset() {
  if (!s_arena) {
    // 動的確保は初期化時のみ (ui::create → theme_apply → ここ)。
    s_arena = static_cast<uint8_t*>(
        heap_caps_malloc(kThemeArenaSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  s_arena_used = kGlobalOff;
  // 画面ブロックも全解放 (適用直後に画面は組み直される)。
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

// <root>/<id>.zip を開いてエントリ表を返す。開けなければ cnt<0。
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

// zip 内エントリのデータオフセットを返す。
bool zip_entry_off(FILE* f, const watch::ThemePackageEntry& e,
                   uint32_t* off) {
  ZipCtx zc{f};
  std::fseek(f, 0, SEEK_END);
  watch::ThemeZipSrc z{&zc, &zip_at,
                       static_cast<uint32_t>(std::ftell(f))};
  return watch::theme_zipfile_data_offset(z, e, off);
}

}  // namespace

// id/name を zip→旧dir の順で探して stat。
bool theme_entry_stat(const char* id, const char* name, uint32_t* out_size) {
  if (!id || !name || !out_size) return false;
  // zip
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
  // 旧式 <id>/<name>
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
  // zip
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
      // zip に無かった場合は旧 dir へ続く
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

// ---- 画面背景ブロック ----

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
  return static_cast<uint8_t*>(
      heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}
void theme_tmp_free(uint8_t* p) { heap_caps_free(p); }

}  // namespace ui::port
