// theme_store.cpp — /assets littlefs 上のテーマ資産管理。
//   zip は丸ごとメモリに持たず、littlefs 上の .theme.bulk から
//   セントラルディレクトリ→各エントリをストリーミングで読んで展開する
//   (PSRAM のテーマ用アリーナは現テーマの画像が使っているので触らない)。
#include "theme_store/theme_store.hpp"

#include <sys/stat.h>

#include <cstdio>
#include <cstring>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "watch/theme/manifest.hpp"
#include "watch/theme/package.hpp"

namespace theme_store {
namespace {

constexpr const char* TAG = "theme_store";
constexpr const char* kRoot = "/assets";
constexpr const char* kThemesDir = "/assets/themes";
constexpr const char* kBulkFile = "/assets/.theme.bulk";
constexpr uint32_t kPkgMax = 3 * 1024 * 1024;
constexpr int kMaxEntries = watch::kThemePackageMaxEntries;
constexpr size_t kManifestMax = 32 * 1024;
constexpr size_t kCopyBufSize = 4096;
// スロットごとの画像サイズ上限 (docs/theme-format.md)。
constexpr uint16_t kMaxDim[watch::kThemeImageSlots][2] = {
    {410, 502}, {240, 360}, {410, 320}};

FILE* s_f = nullptr;          // 受信中の .theme.bulk
uint32_t s_size = 0;          // 期待サイズ
char s_pending[32] = {};      // 適用待ち theme id (volatile 共有)
volatile bool s_has_pending = false;

uint16_t le16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) |
                               (static_cast<uint32_t>(p[3]) << 24));
}

struct Entry {
  char name[48];
  uint32_t lho;
  uint32_t size;
  uint32_t crc;
};

// local file header からデータ位置を取る。
bool data_offset(FILE* f, uint32_t lho, const char* name, uint32_t* off) {
  uint8_t h[30];
  if (std::fseek(f, lho, SEEK_SET) != 0 || std::fread(h, 1, 30, f) != 30) {
    return false;
  }
  if (le32(h) != 0x04034b50u || le16(h + 8) != 0) return false;
  const uint16_t nl = le16(h + 26);
  const uint16_t el = le16(h + 28);
  const size_t nn = std::strlen(name);
  if (nl != nn) return false;
  char buf[48];
  if (std::fread(buf, 1, nn, f) != nn || std::memcmp(buf, name, nn) != 0) {
    return false;
  }
  *off = lho + 30 + nl + el;
  return true;
}

// NimBLE タスク上だけで動くため、大きいバッファは static に置いて
// スタックを食わない (単一スレッド前提で再入なし)。
uint8_t s_tail[4096];
Entry s_list[kMaxEntries];

// セントラルディレクトリを走査して Entry 表を作る。
int list_entries(FILE* f, Entry* out, int cap) {
  // EOCD を末尾から探す (末尾 4KB だけ見る — 自作 zip はコメント無し)。
  uint8_t* tail = s_tail;
  if (std::fseek(f, 0, SEEK_END) != 0) return -1;
  const long fsz = std::ftell(f);
  const long tn = fsz < static_cast<long>(sizeof(tail))
                      ? fsz
                      : static_cast<long>(sizeof(tail));
  if (tn < 22) return -1;
  if (std::fseek(f, fsz - tn, SEEK_SET) != 0 ||
      std::fread(tail, 1, tn, f) != static_cast<size_t>(tn)) {
    return -1;
  }
  const uint8_t* eocd = watch::theme_package_eocd(tail, tn);
  if (!eocd) return -1;
  const uint16_t entries = le16(eocd + 10);
  const uint32_t cd_off = le32(eocd + 16);
  const uint32_t cd_size = le32(eocd + 12);
  if (entries == 0 || entries > cap) return -1;
  if (cd_off + cd_size > static_cast<uint32_t>(fsz)) return -1;

  uint8_t rec[46];
  char name[48];
  uint32_t pos = cd_off;
  int count = 0;
  for (int i = 0; i < entries; ++i) {
    if (std::fseek(f, pos, SEEK_SET) != 0 ||
        std::fread(rec, 1, 46, f) != 46) {
      return -1;
    }
    if (le32(rec) != 0x02014b50u) return -1;
    const uint16_t method = le16(rec + 10);
    const uint16_t nl = le16(rec + 28);
    const uint16_t el = le16(rec + 30);
    const uint16_t cl = le16(rec + 32);
    if (method != 0 || le32(rec + 20) != le32(rec + 24)) return -1;
    if (nl == 0 || nl >= sizeof(name) ||
        std::fread(name, 1, nl, f) != nl) {
      return -1;
    }
    name[nl] = '\0';
    if (!watch::theme_package_name_ok(name, nl)) return -1;
    Entry& e = out[count++];
    std::strncpy(e.name, name, sizeof(e.name) - 1);
    e.lho = le32(rec + 42);
    e.size = le32(rec + 24);
    e.crc = le32(rec + 16);
    if (e.lho + 30 > static_cast<uint32_t>(fsz)) return -1;
    pos += 46 + nl + el + cl;
    if (pos > cd_off + cd_size) return -1;
  }
  return count;
}

const Entry* find_entry(const Entry* list, int count, const char* name) {
  for (int i = 0; i < count; ++i) {
    if (std::strcmp(list[i].name, name) == 0) return &list[i];
  }
  return nullptr;
}

// エントリを dest へストリーミングコピーしつつ crc32 を検算する。
bool copy_entry(FILE* src, const Entry& e, const char* dest) {
  uint32_t off = 0;
  if (!data_offset(src, e.lho, e.name, &off)) return false;
  FILE* d = std::fopen(dest, "wb");
  if (!d) return false;
  if (std::fseek(src, off, SEEK_SET) != 0) {
    std::fclose(d);
    return false;
  }
  static uint8_t buf[kCopyBufSize];
  uint32_t crc = 0xFFFFFFFFu;
  // crc32 の中間計算 (theme_crc32 は完成形なので自前ループ)。
  uint32_t left = e.size;
  bool ok = true;
  while (left > 0) {
    const uint32_t n = left < sizeof(buf) ? left : sizeof(buf);
    if (std::fread(buf, 1, n, src) != n) {
      ok = false;
      break;
    }
    for (uint32_t i = 0; i < n; ++i) {
      // テーブルは package.cpp と同じ多項式の簡易ループ。
      crc ^= buf[i];
      for (int k = 0; k < 8; ++k) {
        crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
      }
    }
    if (std::fwrite(buf, 1, n, d) != n) {
      ok = false;
      break;
    }
    left -= n;
  }
  std::fclose(d);
  if (ok && (crc ^ 0xFFFFFFFFu) != e.crc) ok = false;
  if (!ok) std::remove(dest);
  return ok;
}

// .theme.bulk を検証して /assets/themes/<id>/ へ展開。id を out に。
bool install(const char* tmp, char* out_id, size_t cap) {
  FILE* f = std::fopen(tmp, "rb");
  if (!f) return false;
  Entry* list = s_list;
  const int cnt = list_entries(f, list, kMaxEntries);
  if (cnt < 0) {
    ESP_LOGW(TAG, "bad zip");
    std::fclose(f);
    return false;
  }
  const Entry* me = find_entry(list, cnt, "manifest.cbor");
  if (!me || me->size == 0 || me->size > kManifestMax) {
    std::fclose(f);
    return false;
  }
  uint32_t moff = 0;
  static uint8_t mbuf[kManifestMax];
  if (!data_offset(f, me->lho, me->name, &moff) ||
      std::fseek(f, moff, SEEK_SET) != 0 ||
      std::fread(mbuf, 1, me->size, f) != me->size) {
    std::fclose(f);
    return false;
  }
  watch::ThemeManifest m;
  if (watch::theme_manifest_parse(mbuf, me->size, &m) !=
      watch::ThemeManifestError::kOk) {
    ESP_LOGW(TAG, "bad manifest");
    std::fclose(f);
    return false;
  }

  // /assets/themes/<id>/
  char dir[96];
  std::snprintf(dir, sizeof(dir), "%s/%s", kThemesDir, m.id);
  mkdir(kThemesDir, 0777);  // 既存なら失敗するだけ
  mkdir(dir, 0777);

  // manifest.cbor 自体も置く (適用時に再読するため)。
  char dest[128];
  std::snprintf(dest, sizeof(dest), "%s/manifest.cbor", dir);
  {
    FILE* d = std::fopen(dest, "wb");
    if (!d || std::fwrite(mbuf, 1, me->size, d) != me->size) {
      if (d) std::fclose(d);
      std::fclose(f);
      return false;
    }
    std::fclose(d);
  }

  // 宣言された画像を展開する。
  uint64_t img_bytes = 0;
  for (int s = 0; s < watch::kThemeImageSlots; ++s) {
    if (!(m.image_set & (1u << s))) continue;
    const Entry* e = find_entry(list, cnt, m.image[s]);
    if (!e) {
      ESP_LOGW(TAG, "missing image %s", m.image[s]);
      continue;  // スロットだけ欠け (テーマ自体は入れる)
    }
    // ヘッダ 12B + size 一致を検査してからコピー。
    uint32_t off = 0;
    uint8_t head[12];
    if (!data_offset(f, e->lho, e->name, &off) ||
        std::fseek(f, off, SEEK_SET) != 0 ||
        std::fread(head, 1, 12, f) != 12 ||
        !watch::theme_image_check(head, e->size, kMaxDim[s][0],
                                  kMaxDim[s][1])) {
      ESP_LOGW(TAG, "bad image %s", e->name);
      continue;
    }
    std::snprintf(dest, sizeof(dest), "%s/%s", dir, e->name);
    if (!copy_entry(f, *e, dest)) {
      ESP_LOGW(TAG, "copy/crc fail %s", e->name);
      continue;
    }
    img_bytes += e->size;
    if (img_bytes > kPkgMax) break;
  }
  std::fclose(f);
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
  // 再開: 同じ .theme.bulk の続きなら append。
  // (BulkReceiver が next_offset で再開位置を教えるので、ここでは
  //  既存ファイルがあればそれを使い、なければ新規に作る)
  if (!s_f) {
    s_f = std::fopen(kBulkFile, "r+b");
    if (!s_f) s_f = std::fopen(kBulkFile, "w+b");
    if (!s_f) return false;
  }
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
