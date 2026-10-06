// theme/zipfile.cpp — コールバック経由でファイル上の stored-zip を読む。
// 末尾の EOCD → セントラルディレクトリ → 各 local header という
// 通常の zip 手順。展開はしない (エントリのオフセットだけ返す)。
#include "watch/theme/zipfile.hpp"

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

}  // namespace

int theme_zipfile_list(const ThemeZipSrc& z, ThemePackageEntry* out, int cap,
                       uint8_t* tail_buf, uint32_t tail_cap) {
  if (!z.at || !out || cap < 1 || !tail_buf || tail_cap < 22) return -1;
  const uint32_t tn =
      z.size < tail_cap ? z.size : tail_cap;
  if (tn < 22) return -1;
  if (!z.at(z.user, z.size - tn, tail_buf, tn)) return -1;
  const uint8_t* eocd = theme_package_eocd(tail_buf, tn);
  if (!eocd) return -1;
  const uint16_t entries = le16(eocd + 10);
  const uint32_t cd_off = le32(eocd + 16);
  const uint32_t cd_size = le32(eocd + 12);
  if (entries == 0 || entries > cap) return -1;
  if (cd_off + cd_size > z.size) return -1;

  uint8_t rec[46];
  char name[48];
  uint32_t pos = cd_off;
  int count = 0;
  for (int i = 0; i < entries; ++i) {
    if (!z.at(z.user, pos, rec, 46) || le32(rec) != 0x02014b50u) return -1;
    const uint16_t method = le16(rec + 10);
    const uint16_t nl = le16(rec + 28);
    const uint16_t el = le16(rec + 30);
    const uint16_t cl = le16(rec + 32);
    if (method != 0 || le32(rec + 20) != le32(rec + 24)) return -1;
    if (nl == 0 || nl >= sizeof(name) ||
        !z.at(z.user, pos + 46, reinterpret_cast<uint8_t*>(name), nl)) {
      return -1;
    }
    name[nl] = '\0';
    if (!theme_package_name_ok(name, nl)) return -1;
    ThemePackageEntry& e = out[count++];
    std::memcpy(e.name, name, nl + 1);  // nl < sizeof(name) 確認済み
    e.offset = le32(rec + 42);
    e.size = le32(rec + 24);
    e.crc32 = le32(rec + 16);
    if (e.offset + 30 > z.size) return -1;
    pos += 46 + nl + el + cl;
    if (pos > cd_off + cd_size) return -1;
  }
  return count;
}

bool theme_zipfile_data_offset(const ThemeZipSrc& z,
                               const ThemePackageEntry& e, uint32_t* off) {
  uint8_t h[30];
  if (!z.at(z.user, e.offset, h, 30) || le32(h) != 0x04034b50u ||
      le16(h + 8) != 0) {
    return false;
  }
  const uint16_t nl = le16(h + 26);
  const uint16_t el = le16(h + 28);
  const size_t nn = std::strlen(e.name);
  if (nl != nn) return false;
  char buf[48];
  if (!z.at(z.user, e.offset + 30, reinterpret_cast<uint8_t*>(buf), nn) ||
      std::memcmp(buf, e.name, nn) != 0) {
    return false;
  }
  const uint32_t o = e.offset + 30 + nl + el;
  if (o + e.size > z.size) return false;
  if (off) *off = o;
  return true;
}

}  // namespace watch
