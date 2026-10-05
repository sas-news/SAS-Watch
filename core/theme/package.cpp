// theme/package.cpp — stored zip (圧縮無し) の最小リーダと
// .bin (LVGL 画像) ヘッダ検査。miniz/dep を持たず読む。
#include "watch/theme/package.hpp"

#include <cstring>

namespace watch {
namespace {

uint16_t le16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) |
                               (static_cast<uint32_t>(p[3]) << 24));
}

bool entry_name_ok(const char* s, size_t n) {
  if (n == 0 || n >= 48) return false;
  // ディレクトリ ("dir/"), 親参照 (".."), 絶対パスを弾く。
  for (size_t i = 0; i < n; ++i) {
    const char c = s[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '_' || c == '-';
    if (!ok) return false;
  }
  if (n >= 2 && s[0] == '.' && s[1] == '.') return false;
  return true;
}

}  // namespace

int theme_package_list(const uint8_t* zip, size_t n, ThemePackageEntry* out,
                       int cap) {
  if (!zip || n < 22 || !out || cap < 1) return -1;

  // EOCD (0x06054b50) を末尾から探す (コメント最大 64KB + 22)。
  const size_t window = n < 66000 ? n - 22 : 66000 - 22;
  const uint8_t* eocd = nullptr;
  for (size_t i = 0; i <= window; ++i) {
    const uint8_t* p = zip + (n - 22) - i;
    if (le32(p) == 0x06054b50u) {
      eocd = p;
      break;
    }
  }
  if (!eocd) return -1;

  const uint16_t entries = le16(eocd + 10);
  const uint32_t cd_off = le32(eocd + 16);
  const uint32_t cd_size = le32(eocd + 12);
  if (entries == 0 || entries > cap) return -1;
  if (cd_off + cd_size > n || cd_off >= n) return -1;

  const uint8_t* p = zip + cd_off;
  const uint8_t* cd_end = p + cd_size;
  int count = 0;
  for (int i = 0; i < entries; ++i) {
    if (p + 46 > cd_end || le32(p) != 0x02014b50u) return -1;
    const uint16_t method = le16(p + 10);
    const uint16_t namelen = le16(p + 28);
    const uint16_t extralen = le16(p + 30);
    const uint16_t commentlen = le16(p + 32);
    const uint32_t crc = le32(p + 16);
    const uint32_t csize = le32(p + 20);
    const uint32_t usize = le32(p + 24);
    const uint32_t lho = le32(p + 42);
    if (method != 0 || csize != usize) return -1;  // stored のみ
    if (p + 46 + namelen > cd_end) return -1;
    if (!entry_name_ok(reinterpret_cast<const char*>(p + 46), namelen)) {
      return -1;
    }
    // local header 範囲チェック (データ本体は data() で再検査)。
    if (lho + 30 > n || lho >= n) return -1;
    ThemePackageEntry& e = out[count++];
    std::memcpy(e.name, p + 46, namelen);
    e.name[namelen] = '\0';
    e.offset = lho;
    e.size = usize;
    e.crc32 = crc;
    p += 46 + namelen + extralen + commentlen;
  }
  return count;
}

const uint8_t* theme_package_data(const uint8_t* zip, size_t n,
                                  const ThemePackageEntry* e) {
  if (!zip || !e) return nullptr;
  const uint8_t* lh = zip + e->offset;
  if (e->offset + 30 > n || le32(lh) != 0x04034b50u) return nullptr;
  if (le16(lh + 8) != 0) return nullptr;  // method = stored
  const uint16_t namelen = le16(lh + 26);
  const uint16_t extralen = le16(lh + 28);
  const size_t name_n = std::strlen(e->name);
  if (namelen != name_n ||
      std::memcmp(lh + 30, e->name, name_n) != 0) {
    return nullptr;
  }
  const uint8_t* d = lh + 30 + namelen + extralen;
  if (d + e->size > zip + n) return nullptr;
  return d;
}

const ThemePackageEntry* theme_package_find(const ThemePackageEntry* list,
                                            int count, const char* name) {
  if (!list || !name) return nullptr;
  for (int i = 0; i < count; ++i) {
    if (std::strcmp(list[i].name, name) == 0) return &list[i];
  }
  return nullptr;
}

namespace {
struct CrcTable {
  uint32_t v[256];
  constexpr CrcTable() : v{} {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
      }
      v[i] = c;
    }
  }
};
constexpr CrcTable kCrc{};
}  // namespace

uint32_t theme_crc32(const uint8_t* p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    c = kCrc.v[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}

bool theme_image_check(const uint8_t* bin, size_t n, uint16_t max_w,
                       uint16_t max_h) {
  if (!bin || n < 12) return false;
  if (bin[0] != 0x19) return false;  // LV_IMAGE_HEADER_MAGIC
  const uint8_t cf = bin[1];
  if (cf != 0x12 && cf != 0x14) return false;  // RGB565 / RGB565A8
  const uint16_t w = le16(bin + 4);
  const uint16_t h = le16(bin + 6);
  const uint16_t stride = le16(bin + 8);
  if (w == 0 || h == 0 || w > max_w || h > max_h) return false;
  if (stride != static_cast<uint16_t>(w * 2)) return false;
  const uint32_t per_px = (cf == 0x14) ? 3u : 2u;
  const uint64_t data_size = static_cast<uint64_t>(w) * h * per_px;
  if (data_size > 0xFFFFFFFFull || n != 12 + data_size) return false;
  return true;
}

void theme_image_size(const uint8_t* bin, uint16_t* w, uint16_t* h) {
  if (w) *w = le16(bin + 4);
  if (h) *h = le16(bin + 6);
}

}  // namespace watch
