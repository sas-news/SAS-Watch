// esp_port.cpp — ui::port の ESP-IDF 実装。
// LVGL ロック = lvgl_port_lock、クリティカル = portMUX、
// 電池 = board::pmic、再起動 = esp_restart。
#include "ui/port.hpp"

#include "audio/audio.hpp"
#include "board/board.hpp"
#include "board/power_consts.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_idf_version.h"
#include "esp_app_desc.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
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

// TODO(hw): 実機で確認 — テーマ作業領域は PSRAM に確保する。
// 資産領域はテーマごとの見積もり (theme_res::assets_estimate) 分だけ
// 確保する。常時 4MiB だと残り PSRAM が ~40KB しか無く、描画レイヤー・
// fopen のロック・lodepng が内蔵 RAM に溢れて abort する (cute で実機観測)。
// 画面背景用ブロック (640KiB×2、遷移中に新旧画面が同時に持つ) は
// アリーナとは別個に確保する — T24 の binfont (~1.4MB 常駐) で
// PSRAM が分断されると、資産+ブロックの単一確保 (~3MB) が連続確保
// できず、mame の資産が欠ける。分割なら隙間に入る。
constexpr uint32_t kScreenBlocks = 2;
constexpr uint32_t kAssetCap = 3 * 1024 * 1024;  // 資産領域の上限
uint8_t* s_arena = nullptr;
uint32_t s_arena_size = 0;   // 資産領域だけのサイズ
uint32_t s_arena_used = 0;
uint8_t* s_blk[kScreenBlocks] = {};
bool s_blk_used[kScreenBlocks] = {};

const char* theme_assets_root() { return "/assets/themes"; }

// 呼び出し時点でアリーナの中身は全部 dead (画面は先に落としてある)。
// 失敗すると以後の資産ロードは全部 nullptr → theme_apply が標準に落ちる。
void theme_assets_reset(uint32_t asset_bytes) {
  if (s_arena) {
    heap_caps_free(s_arena);
    s_arena = nullptr;
  }
  for (auto& b : s_blk) {
    if (b) {
      heap_caps_free(b);
      b = nullptr;
    }
  }
  for (auto& u : s_blk_used) u = false;
  if (asset_bytes > kAssetCap) asset_bytes = kAssetCap;
  s_arena_size = asset_bytes;
  if (!s_arena_size) return;
  // 画面ブロックを資産より先に確保する — 640KiB×2 が無いと
  // 画面遷移中に背景スキンが片方消える。組み込みテーマ
  // (asset_bytes=0) は画面スキンを使わないので取らない。
  for (auto& b : s_blk) {
    b = static_cast<uint8_t*>(heap_caps_malloc(
        kThemeScreenBlockSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!b) break;
  }
  s_arena = static_cast<uint8_t*>(
      heap_caps_malloc(s_arena_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!s_arena) {
    // PSRAM の分断で見積もり分の連続確保に失敗したら、最大の連続
    // ブロックで縮めて開き直す (大きい資産から順に欠けるだけで、
    // s_arena=NULL の全滅よりはマシ)。
    const uint32_t largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if (largest > 128 * 1024) {
      s_arena_size = largest & ~15u;
      s_arena = static_cast<uint8_t*>(heap_caps_malloc(
          s_arena_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (s_arena) {
      ESP_LOGW("theme", "arena shrink %lu -> %lu (fragmented)",
               (unsigned long)asset_bytes,
               (unsigned long)s_arena_size);
    }
  }
  if (!s_arena) {
    s_arena_size = 0;
    ESP_LOGW("theme", "arena alloc %lu failed, psram free=%lu largest=%lu",
             (unsigned long)asset_bytes,
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned long)heap_caps_get_largest_free_block(
                 MALLOC_CAP_SPIRAM));
  }
  s_arena_used = 0;
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

// dst が PSRAM (テーマアリーナ) のまま fread すると、esp_flash_read が
// 「cache 停止中に書けるのは DRAM だけ」という制約から内部 RAM の一時
// バッファを都度確保/解放する。起動直後の細かく割れたヒープでこの
// alloc が TLSF の空きリスト破壊を踏んでいた (mame 適用で boot loop を
// 実機観測)。DRAM の小さいバッファに刻んで読み、呼び出し側 dst へ
// 転写すれば direct_read 経路になり一時バッファは確保されない。
bool staged_fread(FILE* f, uint8_t* dst, uint32_t n) {
  // 4KB は内蔵 DRAM の節約と読み込み効率の均衡点
  // (これ以上大きい静的バッファは BLE_INIT の ~30KB 連続確保を破る)。
  static uint8_t s_stage[4 * 1024];  // DRAM (.bss)
  uint32_t done = 0;
  while (done < n) {
    const uint32_t m =
        (n - done) < sizeof(s_stage) ? (n - done) : sizeof(s_stage);
    if (std::fread(s_stage, 1, m, f) != m) return false;
    std::memcpy(dst + done, s_stage, m);
    done += m;
  }
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
             staged_fread(f, dst, e->size);
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
  const bool ok = staged_fread(f, dst, static_cast<uint32_t>(sz));
  std::fclose(f);
  return ok;
}
}

// 先頭 n バイトのみ読む (見積もり用の PNG IHDR 取得など)。
bool theme_entry_head(const char* id, const char* name, uint8_t* dst,
                      uint32_t n) {
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
      if (e) {
        uint32_t off = 0;
        const uint32_t rn = n < e->size ? n : e->size;
        ok = zip_entry_off(f, *e, &off) &&
             std::fseek(f, static_cast<long>(off), SEEK_SET) == 0 &&
             std::fread(dst, 1, rn, f) == rn;
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
  const bool ok = std::fread(dst, 1, n, f) == n;
  std::fclose(f);
  return ok;
}
}

bool theme_asset_load(const char* id, const char* name, const uint8_t** out,
                      uint32_t* out_len) {
  if (!s_arena || !out || !out_len) return false;
  uint32_t sz = 0;
  if (!theme_entry_stat(id, name, &sz) || sz == 0) return false;
  if (static_cast<uint64_t>(s_arena_used) + sz + 8 > s_arena_size) {
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
                      s_arena_size) {
    return nullptr;
  }
  uint8_t* p = s_arena + s_arena_used;
  s_arena_used += (n + 7u) & ~7u;
  return p;
}

// ---- 画面背景ブロック ----

// 640KiB×2 をアリーナとは別に確保する。テーマ期間中は使い回し、
// theme_assets_reset でまとめて解放する。
uint8_t* theme_screen_block() {
  for (uint32_t i = 0; i < kScreenBlocks; ++i) {
    if (s_blk_used[i]) continue;
    if (!s_blk[i]) {
      s_blk[i] = static_cast<uint8_t*>(heap_caps_malloc(
          kThemeScreenBlockSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (!s_blk[i]) return nullptr;
    }
    s_blk_used[i] = true;
    return s_blk[i];
  }
  return nullptr;
}

void theme_screen_free(uint8_t* p) {
  if (!p) return;
  for (uint32_t i = 0; i < kScreenBlocks; ++i) {
    if (s_blk[i] == p) {
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

// 内蔵フォント (T24): app バイナリから外した日本語フォントを
// /assets/fonts/jp_<px>.bin から読み、lv_binfont で生成する。
// ローダは glyph 表を LVGL ヒープ (PSRAM) に全複製するので、
// 元バッファは create 後に解放してよい。バイナリ内に残ったのは
// ASCII+かなの最小フォントだけで、ここが失敗すると漢字が出ない。
const lv_font_t* builtin_font_load(uint8_t px) {
  constexpr uint8_t kSizes[3] = {20, 26, 32};
  static lv_font_t* s_fonts[3] = {};
  static bool s_tried[3] = {};
  int i = -1;
  for (int k = 0; k < 3; ++k) {
    if (px == kSizes[k]) i = k;
  }
  if (i < 0) return nullptr;
  if (s_tried[i]) return s_fonts[i];
  s_tried[i] = true;

  char path[64];
  std::snprintf(path, sizeof(path), "/assets/fonts/jp_%u.bin", px);
  FILE* f = std::fopen(path, "rb");
  if (!f) {
    ESP_LOGW("font", "%s なし (assets 未更新?)", path);
    return nullptr;
  }
  std::fseek(f, 0, SEEK_END);
  const long sz = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (sz <= 0) {
    std::fclose(f);
    return nullptr;
  }
  // ローダは作業バッファ + LVGL ヒープへの複製で ~2 倍の PSRAM が
  // 必要。足りない時に load_glyph 内で OOM すると NULL 書き込みで
  // LVGL タスクが死ぬので、先に残量を見て最小フォントに逃がす。
  if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) <
      static_cast<uint32_t>(sz) * 2) {
    std::fclose(f);
    ESP_LOGW("font", "%s skip: psram tight (%ldB file)", path, sz);
    return nullptr;
  }
  uint8_t* buf = static_cast<uint8_t*>(
      heap_caps_malloc(static_cast<uint32_t>(sz),
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!buf || !staged_fread(f, buf, static_cast<uint32_t>(sz))) {
    std::fclose(f);
    heap_caps_free(buf);
    ESP_LOGW("font", "%s read %ld failed", path, sz);
    return nullptr;
  }
  std::fclose(f);
  lv_font_t* lf = lv_binfont_create_from_buffer(buf,
                                              static_cast<uint32_t>(sz));
  // glyph/cmap は全部 LVGL ヒープに複製済み → 元バッファは不要。
  heap_caps_free(buf);
  if (!lf) {
    ESP_LOGW("font", "%s binfont parse failed", path);
    return nullptr;
  }
  s_fonts[i] = lf;
  ESP_LOGI("font", "%s -> binfont %ldB", path, sz);
  return lf;
}

}  // namespace ui::port
