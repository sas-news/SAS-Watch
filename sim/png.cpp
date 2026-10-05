// png.cpp — LVGL の RGB565 フレームバッファを PNG で書き出す最小ライタ。
// zlib (compress2) に依存。行フィルタは 0 (None) 固定。
#include "png.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

#include <zlib.h>

namespace {

void put32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(static_cast<uint8_t>(x >> 24));
  v.push_back(static_cast<uint8_t>(x >> 16));
  v.push_back(static_cast<uint8_t>(x >> 8));
  v.push_back(static_cast<uint8_t>(x));
}

uint32_t crc32_of(const uint8_t* data, size_t len) {
  return static_cast<uint32_t>(crc32(crc32(0, Z_NULL, 0), data,
                                     static_cast<uInt>(len)));
}

void chunk(std::vector<uint8_t>& out, const char type[4],
           const uint8_t* data, size_t len) {
  put32(out, static_cast<uint32_t>(len));
  const size_t at = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data, data + len);
  const uint32_t c = crc32_of(out.data() + at, 4 + len);
  put32(out, c);
}

}  // namespace

bool write_png_rgb565(const char* path, const uint16_t* fb, int w, int h) {
  // 行ごとに filter byte + RGB888。
  std::vector<uint8_t> raw(static_cast<size_t>(h) * (1 + 3 * w));
  for (int y = 0; y < h; ++y) {
    uint8_t* row = raw.data() + static_cast<size_t>(y) * (1 + 3 * w);
    *row++ = 0;  // filter: none
    for (int x = 0; x < w; ++x) {
      const uint16_t p = fb[y * w + x];
      const uint8_t r5 = static_cast<uint8_t>((p >> 11) & 0x1F);
      const uint8_t g6 = static_cast<uint8_t>((p >> 5) & 0x3F);
      const uint8_t b5 = static_cast<uint8_t>(p & 0x1F);
      *row++ = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
      *row++ = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
      *row++ = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
    }
  }

  uLongf zlen = compressBound(static_cast<uLong>(raw.size()));
  std::vector<uint8_t> z(zlen);
  if (compress2(z.data(), &zlen, raw.data(),
                static_cast<uLong>(raw.size()), 9) != Z_OK) {
    return false;
  }

  std::vector<uint8_t> out;
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  out.insert(out.end(), sig, sig + 8);

  uint8_t ihdr[13];
  ihdr[0] = static_cast<uint8_t>(w >> 24);
  ihdr[1] = static_cast<uint8_t>(w >> 16);
  ihdr[2] = static_cast<uint8_t>(w >> 8);
  ihdr[3] = static_cast<uint8_t>(w);
  ihdr[4] = static_cast<uint8_t>(h >> 24);
  ihdr[5] = static_cast<uint8_t>(h >> 16);
  ihdr[6] = static_cast<uint8_t>(h >> 8);
  ihdr[7] = static_cast<uint8_t>(h);
  ihdr[8] = 8;   // bit depth
  ihdr[9] = 2;   // truecolor
  ihdr[10] = 0;  // compression
  ihdr[11] = 0;  // filter
  ihdr[12] = 0;  // interlace
  chunk(out, "IHDR", ihdr, sizeof(ihdr));
  chunk(out, "IDAT", z.data(), zlen);
  chunk(out, "IEND", nullptr, 0);

  FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  const size_t wr = std::fwrite(out.data(), 1, out.size(), f);
  std::fclose(f);
  return wr == out.size();
}
