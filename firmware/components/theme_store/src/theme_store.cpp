// theme_store.cpp — /assets littlefs 上のテーマ資産管理。
//   受信した zip は展開せず /assets/themes/<id>.zip としてそのまま保持し、
//   エントリは ui 側 (port) がセントラルディレクトリから遅延読みする。
//   旧式 (v1 で <id>/ に展開された資産) は port 側がフォールバックで読み、
//   再インストール時にここで掃除する。
#include "theme_store/theme_store.hpp"

#include <sys/stat.h>

#include <cstdio>
#include <cstring>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "watch/theme/manifest.hpp"
#include "watch/theme/package.hpp"
#include "watch/theme/zipfile.hpp"

namespace theme_store {
namespace {

constexpr const char* TAG = "theme_store";
constexpr const char* kRoot = "/assets";
constexpr const char* kThemesDir = "/assets/themes";
constexpr const char* kBulkFile = "/assets/.theme.bulk";
// 4 MiB: assets パーティション 6MiB − 他データ/旧資産の予約 (~2MiB)。
// zip を展開しないので「staging+展開済み」の2倍占有が消え、上限はほぼ
// パーティションの空きそのものになる (docs/theme-format.md 予算表)。
constexpr uint32_t kPkgMax = 4 * 1024 * 1024;
constexpr int kMaxEntries = watch::kThemePackageMaxEntries;
constexpr size_t kManifestMax = 32 * 1024;

FILE* s_f = nullptr;          // 受信中の .theme.bulk
uint32_t s_size = 0;          // 期待サイズ
char s_pending[32] = {};      // 適用待ち theme id (volatile 共有)
volatile bool s_has_pending = false;

// NimBLE タスク上だけで動くため、大きいバッファは static に置いて
// スタックを食わない (単一スレッド前提で再入なし)。
uint8_t s_tail[4096];
watch::ThemePackageEntry s_list[kMaxEntries];

bool file_at(void* user, uint32_t off, uint8_t* dst, uint32_t n) {
  FILE* f = static_cast<FILE*>(user);
  return std::fseek(f, static_cast<long>(off), SEEK_SET) == 0 &&
         std::fread(dst, 1, n, f) == n;
}

// .theme.bulk を検証して /assets/themes/<id>.zip へ rename する。
// 同じ id の旧式展開ディレクトリ (<id>/) はエントリ単位で掃除する。
bool install(const char* tmp, char* out_id, size_t cap) {
  FILE* f = std::fopen(tmp, "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long fsz = std::ftell(f);
  if (fsz <= 0) {
    std::fclose(f);
    return false;
  }
  watch::ThemeZipSrc z{f, &file_at, static_cast<uint32_t>(fsz)};
  const int cnt = watch::theme_zipfile_list(z, s_list, kMaxEntries, s_tail,
                                            sizeof(s_tail));
  if (cnt < 0) {
    ESP_LOGW(TAG, "bad zip");
    std::fclose(f);
    return false;
  }
  const watch::ThemePackageEntry* me =
      watch::theme_package_find(s_list, cnt, "manifest.cbor");
  if (!me || me->size == 0 || me->size > kManifestMax) {
    std::fclose(f);
    return false;
  }
  static uint8_t mbuf[32 * 1024];
  uint32_t moff = 0;
  if (!watch::theme_zipfile_data_offset(z, *me, &moff) ||
      !z.at(z.user, moff, mbuf, me->size)) {
    std::fclose(f);
    return false;
  }
  static watch::ThemeManifest m;  // NimBLE タスクのスタック節約のため static
  if (watch::theme_manifest_parse(mbuf, me->size, &m) !=
      watch::ThemeManifestError::kOk) {
    ESP_LOGW(TAG, "bad manifest");
    std::fclose(f);
    return false;
  }

  // /assets/themes/<id>.zip に rename (zip は展開しない)。
  char dest[96];
  std::snprintf(dest, sizeof(dest), "%s/%s.zip", kThemesDir, m.id);
  std::fclose(f);
  mkdir(kThemesDir, 0777);  // 既存なら失敗するだけ
  std::remove(dest);        // 上書き (rename は既存ファイルを置き換えない実装がある)
  if (std::rename(tmp, dest) != 0) {
    ESP_LOGW(TAG, "rename %s failed", dest);
    return false;
  }

  // v1 形式で展開済みの同名ディレクトリがあれば掃除する
  // (zip 内エントリ名 + manifest.cbor を消し、空なら dir も消す)。
  char dir[96];
  std::snprintf(dir, sizeof(dir), "%s/%s", kThemesDir, m.id);
  char p[160];
  std::snprintf(p, sizeof(p), "%s/manifest.cbor", dir);
  std::remove(p);
  for (int i = 0; i < cnt; ++i) {
    std::snprintf(p, sizeof(p), "%s/%s", dir, s_list[i].name);
    std::remove(p);
  }
  rmdir(dir);  // 残ファイルがあれば失敗するだけ

  std::strncpy(out_id, m.id, cap - 1);
  out_id[cap - 1] = '\0';
  return true;
}

}  // namespace

bool init() {
  esp_vfs_littlefs_conf_t conf = {};
  conf.base_path = kRoot;
  conf.partition_label = "assets";
  conf.format_if_mount_failed = true;  // TODO(hw): 実機で確認
  conf.dont_mount = false;
  const esp_err_t err = esp_vfs_littlefs_register(&conf);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
    return false;
  }
  mkdir(kThemesDir, 0777);
  ESP_LOGI(TAG, "mounted %s", kRoot);
  return true;
}

bool bulk_begin(uint16_t, const char* kind, uint32_t size, void*) {
  if (!kind || std::strcmp(kind, "theme") != 0) return false;
  if (size == 0 || size > kPkgMax) return false;
  // begin は常に「新しい転送」。再開 (同じ id/size/sha256 の BULK_START 再送)
  // は BulkReceiver 側が begin を呼ばず受信位置を返すので、ここに来た時点で
  // 前の .theme.bulk は残ってるだけのゴミ → 必ず truncate して取り直す。
  // (切れた転送は電話側が最初から送り直す)
  if (s_f) {
    std::fclose(s_f);
    s_f = nullptr;
  }
  s_f = std::fopen(kBulkFile, "w+b");
  if (!s_f) return false;
  s_size = size;
  return true;
}

bool bulk_write(uint16_t, uint32_t offset, const uint8_t* data, size_t len,
                void*) {
  if (!s_f || !data || offset + len > s_size) return false;
  if (std::fseek(s_f, offset, SEEK_SET) != 0) return false;
  return std::fwrite(data, 1, len, s_f) == len;
}

bool bulk_commit(uint16_t, void*) {
  if (s_f) {
    std::fclose(s_f);
    s_f = nullptr;
  }
  char id[32];
  const bool ok = install(kBulkFile, id, sizeof(id));
  std::remove(kBulkFile);
  if (ok) set_pending_theme(id);
  ESP_LOGI(TAG, "theme install %s", ok ? id : "failed");
  return ok;
}

void bulk_abort(uint16_t, void*) {
  if (s_f) {
    std::fclose(s_f);
    s_f = nullptr;
  }
  std::remove(kBulkFile);
}

void set_pending_theme(const char* id) {
  if (!id || !watch::theme_id_ok(id)) return;
  std::strncpy(s_pending, id, sizeof(s_pending) - 1);
  s_pending[sizeof(s_pending) - 1] = '\0';
  s_has_pending = true;
}

bool take_pending_theme(char* out, size_t cap) {
  if (!s_has_pending) return false;
  s_has_pending = false;
  std::strncpy(out, s_pending, cap - 1);
  out[cap - 1] = '\0';
  return true;
}

}  // namespace theme_store
