// theme_res.cpp — テーマ資産の遅延ロード (docs/theme-format.md v2)。
// .bin は検査してそのまま、.png は lodepng で RGB565A8 に一度だけ変換。
// フォントは lv_binfont_create_from_buffer (バッファはアリーナで保持)。
#include "theme_res.hpp"

#include <cstring>

// lodepng.h は extern "C" の内側で <vector>/<string> を include するため
// C++ から直接読めない。使う関数だけここで宣言する (lvgl が lodepng.c を
// コンパイルするのでリンクは通る)。
extern "C" {
unsigned lodepng_decode32(unsigned char** out, unsigned* w, unsigned* h,
                          const unsigned char* in, size_t inlen);
const char* lodepng_error_text(unsigned code);
}
#include "ui/port.hpp"
#include "watch/theme/package.hpp"

namespace theme_res {
namespace {

// PNG ソースファイルの上限 (デコード先の cap は呼び出し側が決める)。
constexpr uint32_t kPngSrcMax = 1024 * 1024;
constexpr uint16_t kAlphaOpaque = 255;

bool ends_png(const char* name) {
  const size_t n = std::strlen(name);
  return n >= 4 && std::strcmp(name + n - 4, ".png") == 0;
}

// RGB565A8 (cf 0x14) のヘッダ + 2 面 (色 + 不透明アルファ) を組み立てる。
// png の rgba を 16bit+alpha に縮める。戻り値: 総バイト数 (0=失敗)。
uint32_t png_to_rgb565a8(const uint8_t* rgba, uint16_t w, uint16_t h,
                         uint8_t* dst, uint32_t cap, lv_image_dsc_t* dsc) {
  const uint32_t px = static_cast<uint32_t>(w) * h;
  const uint64_t total = 12u + px * 3u;
  if (total > cap || total > 0xFFFFFFFFull) return 0;
  uint8_t* c = dst + 12;
  uint8_t* a = c + px * 2;
  for (uint32_t i = 0; i < px; ++i) {
    const uint8_t r = rgba[i * 4 + 0];
    const uint8_t g = rgba[i * 4 + 1];
    const uint8_t b = rgba[i * 4 + 2];
    const uint16_t v = static_cast<uint16_t>(
        ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    c[i * 2] = static_cast<uint8_t>(v);
    c[i * 2 + 1] = static_cast<uint8_t>(v >> 8);
    a[i] = rgba[i * 4 + 3];
  }
  std::memset(dsc, 0, sizeof(*dsc));
  dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
  dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
  dsc->header.w = w;
  dsc->header.h = h;
  dsc->header.stride = static_cast<uint16_t>(w * 2);
  dsc->data_size = px * 3u;
  dsc->data = dst + 12;
  return static_cast<uint32_t>(total);
}

uint32_t decode_bin(const char* id, const char* file, uint8_t* dst,
                    uint32_t cap, uint16_t max_w, uint16_t max_h,
                    lv_image_dsc_t* dsc) {
  uint32_t sz = 0;
  if (!ui::port::theme_entry_stat(id, file, &sz) || sz > cap ||
      !ui::port::theme_entry_read(id, file, dst, sz)) {
    return 0;
  }
  if (!watch::theme_image_check(dst, sz, max_w, max_h)) return 0;
  std::memset(dsc, 0, sizeof(*dsc));
  dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
  dsc->header.cf = (dst[1] == 0x14) ? LV_COLOR_FORMAT_RGB565A8
                                  : LV_COLOR_FORMAT_RGB565;
  uint16_t iw = 0, ih = 0;
  watch::theme_image_size(dst, &iw, &ih);
  dsc->header.w = iw;
  dsc->header.h = ih;
  dsc->header.stride = static_cast<uint16_t>(dsc->header.w * 2);
  dsc->data_size = sz - 12;
  dsc->data = dst + 12;
  return sz;
}

uint32_t decode_png(const char* id, const char* file, uint8_t* dst,
                    uint32_t cap, uint16_t max_w, uint16_t max_h,
                    lv_image_dsc_t* dsc) {
  uint32_t sz = 0;
  if (!ui::port::theme_entry_stat(id, file, &sz) || sz == 0 ||
      sz > kPngSrcMax) {
    return 0;
  }
  uint8_t* src = ui::port::theme_tmp_alloc(sz);
  if (!src) return 0;
  uint32_t out = 0;
  if (ui::port::theme_entry_read(id, file, src, sz)) {
    // LVGL 同梱 lodepng はパッチ済み: *out には RGBA 生配列ではなく
    // lv_draw_buf_t* (ARGB8888, data は PNG の RGBA バイト順) が入る。
    uint8_t* raw = nullptr;
    unsigned w = 0, h = 0;
    // lodepng は lv_draw_buf/lv_malloc 経由 (TODO(hw): 実機で確認 — 410x502 で ~820KB)
    const unsigned err = lodepng_decode32(&raw, &w, &h, src, sz);
    if (err == 0 && raw) {
      lv_draw_buf_t* buf = reinterpret_cast<lv_draw_buf_t*>(raw);
      if (buf->data && w > 0 && w <= max_w && h > 0 && h <= max_h) {
        out = png_to_rgb565a8(buf->data, static_cast<uint16_t>(w),
                              static_cast<uint16_t>(h), dst, cap, dsc);
      }
      lv_draw_buf_destroy(buf);
    }
  }
  ui::port::theme_tmp_free(src);
  return out;
}

// ロードしたフォントの追跡 (fonts_reset でまとめて解放)。
lv_font_t* s_fonts[4] = {};

// グローバル画像 dsc の静的プール (テーマごとに枯れないよう大きめ)。
// データ自体はアリーナなのでプールだけ生きてればよい。
struct DscSlot {
  lv_image_dsc_t d;
  bool used;
};
DscSlot s_dscs[48] = {};

}  // namespace

uint32_t img_decode(const char* id, const char* file, uint8_t* dst,
                    uint32_t cap, uint16_t max_w, uint16_t max_h,
                    lv_image_dsc_t* dsc) {
  if (!id || !file || !dst || !dsc) return 0;
  return ends_png(file)
             ? decode_png(id, file, dst, cap, max_w, max_h, dsc)
             : decode_bin(id, file, dst, cap, max_w, max_h, dsc);
}

const lv_image_dsc_t* img_load(const char* id, const char* file,
                               uint16_t max_w, uint16_t max_h) {
  if (!id || !file) return nullptr;
  // アリーナ確保版: dsc は静的プール、データは
  //   .bin → theme_asset_load でそのまま
  //   .png → theme_tmp でデコードしてから必要分だけアリーナに移す
  DscSlot* slot = nullptr;
  for (auto& d : s_dscs) {
    if (!d.used) {
      slot = &d;
      break;
    }
  }
  if (!slot) return nullptr;
  lv_image_dsc_t* dsc = &slot->d;
  if (!ends_png(file)) {
    const uint8_t* buf = nullptr;
    uint32_t len = 0;
    if (!ui::port::theme_asset_load(id, file, &buf, &len) ||
        !watch::theme_image_check(buf, len, max_w, max_h)) {
      return nullptr;
    }
    std::memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = (buf[1] == 0x14) ? LV_COLOR_FORMAT_RGB565A8
                                    : LV_COLOR_FORMAT_RGB565;
    uint16_t iw = 0, ih = 0;
    watch::theme_image_size(buf, &iw, &ih);
    dsc->header.w = iw;
    dsc->header.h = ih;
    dsc->header.stride = static_cast<uint16_t>(dsc->header.w * 2);
    dsc->data_size = len - 12;
    dsc->data = buf + 12;
    slot->used = true;
    return dsc;
  }
  // .png: tmp にデコード → アリーナへ転写
  uint32_t sz = 0;
  if (!ui::port::theme_entry_stat(id, file, &sz) || sz == 0 ||
      sz > kPngSrcMax) {
    return nullptr;
  }
  uint8_t* src = ui::port::theme_tmp_alloc(sz);
  if (!src) return nullptr;
  const lv_image_dsc_t* ret = nullptr;
  if (ui::port::theme_entry_read(id, file, src, sz)) {
    // decode_png 同様、LVGL 版 lodepng の *out は lv_draw_buf_t*。
    uint8_t* raw = nullptr;
    unsigned w = 0, h = 0;
    const unsigned err = lodepng_decode32(&raw, &w, &h, src, sz);
    if (err == 0 && raw) {
      lv_draw_buf_t* buf = reinterpret_cast<lv_draw_buf_t*>(raw);
      const uint32_t need = static_cast<uint32_t>(w) * h * 3u + 12u;
      if (buf->data && w > 0 && w <= max_w && h > 0 && h <= max_h) {
        uint8_t* dst = ui::port::theme_asset_alloc(need);
        if (dst &&
            png_to_rgb565a8(buf->data, static_cast<uint16_t>(w),
                            static_cast<uint16_t>(h), dst, need, dsc)) {
          slot->used = true;
          ret = dsc;
        }
      }
      lv_draw_buf_destroy(buf);
    }
  }
  ui::port::theme_tmp_free(src);
  return ret;
}

const lv_font_t* font_load(const char* id, const char* file) {
  if (!id || !file) return nullptr;
  // フォントデータはアリーナに保持 (binfont はバッファを直接参照する)。
  const uint8_t* buf = nullptr;
  uint32_t len = 0;
  if (!ui::port::theme_asset_load(id, file, &buf, &len) || len < 16) {
    return nullptr;
  }
  lv_font_t* f = lv_binfont_create_from_buffer(
      const_cast<uint8_t*>(buf), len);
  if (!f) return nullptr;
  for (auto& s : s_fonts) {
    if (!s) {
      s = f;
      break;
    }
  }
  return f;
}

void reset() {
  // binfont フォントを解放してから dsc プールを空にする
  // (呼び出し側がこの直後に ui::port::theme_assets_reset() する)。
  for (auto& s : s_fonts) {
    if (s) lv_binfont_destroy(s);
    s = nullptr;
  }
  for (auto& d : s_dscs) {
    d.used = false;
  }
}

}  // namespace theme_res
