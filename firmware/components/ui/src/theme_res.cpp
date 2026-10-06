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

// ---- v3: skin 9-slice ロード ----

// skin 画像の寸法上限 (header_bar 410x76、ボタン 366x58 などが収まる)。
constexpr uint16_t kSkinMaxW = 410;
constexpr uint16_t kSkinMaxH = 200;
uint32_t s_skin_bytes = 0;

// ファイル全体を tmp にデコードして dsc を組み立てる。
// 戻り値: 解放すべき tmp バッファ (nullptr=失敗)。dsc->data は tmp 内を指す。
uint8_t* load_full_dsc(const char* id, const char* file,
                       lv_image_dsc_t* dsc) {
  uint32_t sz = 0;
  if (!ui::port::theme_entry_stat(id, file, &sz) || sz == 0 ||
      sz > kPngSrcMax) {
    return nullptr;
  }
  const bool png = ends_png(file);
  // .bin はそのまま、.png は RGB565A8 最大サイズで見積もる。
  const uint32_t cap =
      png ? (12u + static_cast<uint32_t>(kSkinMaxW) * kSkinMaxH * 3u) : sz;
  uint8_t* tmp = ui::port::theme_tmp_alloc(cap);
  if (!tmp) return nullptr;
  const uint32_t n =
      png ? decode_png(id, file, tmp, cap, kSkinMaxW, kSkinMaxH, dsc)
          : decode_bin(id, file, tmp, cap, kSkinMaxW, kSkinMaxH, dsc);
  if (!n) {
    ui::port::theme_tmp_free(tmp);
    return nullptr;
  }
  return tmp;
}

// dsc (RGB565A8 or RGB565) の矩形 (rx,ry,rw,rh) をアリーナに
// コピーして独立した画像にする。戻り値: アリーナ内 dsc (nullptr=失敗)。
lv_image_dsc_t* copy_region(const lv_image_dsc_t* src, uint16_t rx,
                            uint16_t ry, uint16_t rw, uint16_t rh) {
  const bool a8 = src->header.cf == LV_COLOR_FORMAT_RGB565A8;
  const uint16_t sw = src->header.w;
  const uint32_t px = static_cast<uint32_t>(rw) * rh;
  const uint32_t need = px * (a8 ? 3u : 2u);
  uint8_t* dst = ui::port::theme_asset_alloc(need);
  if (!dst) return nullptr;
  const uint8_t* csrc = src->data + (static_cast<uint32_t>(ry) * sw + rx) * 2;
  uint8_t* cdst = dst;
  for (uint16_t y = 0; y < rh; ++y) {
    std::memcpy(cdst, csrc, rw * 2);
    cdst += rw * 2;
    csrc += sw * 2;
  }
  if (a8) {
    const uint8_t* asrc =
        src->data + static_cast<uint32_t>(sw) * src->header.h * 2 +
        static_cast<uint32_t>(ry) * sw + rx;
    uint8_t* adst = dst + px * 2;
    for (uint16_t y = 0; y < rh; ++y) {
      std::memcpy(adst, asrc, rw);
      adst += rw;
      asrc += sw;
    }
  }
  s_skin_bytes += need;
  // バッファをアリーナに確保してから dsc をそこに組み立てる
  lv_image_dsc_t* d = reinterpret_cast<lv_image_dsc_t*>(
      ui::port::theme_asset_alloc(sizeof(lv_image_dsc_t)));
  if (!d) return nullptr;
  s_skin_bytes += sizeof(lv_image_dsc_t);
  std::memset(d, 0, sizeof(*d));
  d->header.magic = LV_IMAGE_HEADER_MAGIC;
  d->header.cf = src->header.cf;
  d->header.w = rw;
  d->header.h = rh;
  d->header.stride = static_cast<uint16_t>(rw * 2);
  d->data_size = need;
  d->data = dst;
  return d;
}

// 9-slice 分割して SkinImg (アリーナ内) を返す。slice 全0 なら全体1枚。
const SkinImg* img_load_sliced(const char* id, const char* file,
                               const uint8_t slice[4]) {
  lv_image_dsc_t full;
  uint8_t* tmp = load_full_dsc(id, file, &full);
  if (!tmp) return nullptr;
  const uint16_t w = full.header.w;
  const uint16_t h = full.header.h;
  // slice を画像内に収める (入らなければ描画側と同じく縮める。
  // 角が潰れてもパーツごとベクタに落ちるよりマシ)。
  uint8_t sl[4] = {slice[0], slice[1], slice[2], slice[3]};
  if (static_cast<uint16_t>(sl[0]) + sl[2] > w) {
    sl[0] = static_cast<uint8_t>(w / 2);
    sl[2] = static_cast<uint8_t>(w - sl[0]);
  }
  if (static_cast<uint16_t>(sl[1]) + sl[3] > h) {
    sl[1] = static_cast<uint8_t>(h / 2);
    sl[3] = static_cast<uint8_t>(h - sl[1]);
  }
  SkinImg* si = reinterpret_cast<SkinImg*>(
      ui::port::theme_asset_alloc(sizeof(SkinImg)));
  if (!si) {
    ui::port::theme_tmp_free(tmp);
    return nullptr;
  }
  s_skin_bytes += sizeof(SkinImg);
  std::memset(si->reg, 0, sizeof(si->reg));
  std::memcpy(si->slice, sl, 4);

  if (sl[0] == 0 && sl[1] == 0 && sl[2] == 0 && sl[3] == 0) {
    // 分割なし: 中央領域に全体 (全面伸縮で使う)。
    const lv_image_dsc_t* c = copy_region(&full, 0, 0, w, h);
    if (!c) {
      ui::port::theme_tmp_free(tmp);
      return nullptr;
    }
    si->reg[4] = *c;
  } else {
    const uint16_t xs[4] = {0, sl[0], static_cast<uint16_t>(w - sl[2]), w};
    const uint16_t ys[4] = {0, sl[1], static_cast<uint16_t>(h - sl[3]), h};
    for (int r = 0; r < 3; ++r) {
      for (int cix = 0; cix < 3; ++cix) {
        const int rw = xs[cix + 1] - xs[cix];
        const int rh = ys[r + 1] - ys[r];
        const int idx = r * 3 + cix;
        if (rw <= 0 || rh <= 0) continue;
        const lv_image_dsc_t* d =
            copy_region(&full, xs[cix], ys[r], static_cast<uint16_t>(rw),
                        static_cast<uint16_t>(rh));
        if (!d) {
          ui::port::theme_tmp_free(tmp);
          return nullptr;
        }
        si->reg[idx] = *d;
      }
    }
  }
  ui::port::theme_tmp_free(tmp);
  return si;
}

// SkinSet の静的プール (アリーナ reset と同期して空にする)。
SkinSet s_skin_sets[watch::kThemeSkinPartCount] = {};
uint8_t s_skin_set_used = 0;

}  // namespace

const SkinSet* skin_load(const char* id, const watch::ThemeSkinPart& part) {
  if (!id || !part.img[0] ||
      s_skin_set_used >= watch::kThemeSkinPartCount) {
    return nullptr;
  }
  const SkinImg* n = img_load_sliced(id, part.img, part.slice);
  if (!n) return nullptr;
  SkinSet* ss = &s_skin_sets[s_skin_set_used++];
  ss->img[0] = n;
  for (int i = 0; i < watch::kThemeSkinStateCount; ++i) {
    ss->img[1 + i] = (part.state_set & (1u << i))
                         ? img_load_sliced(id, part.state_img[i], part.slice)
                         : nullptr;
    // state 画像が読めなくても normal にフォールバックするので継続。
  }
  return ss;
}

uint32_t skin_bytes() { return s_skin_bytes; }

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
  // skin も全てアリーナ内なのでインデックスだけ戻す。
  s_skin_set_used = 0;
  s_skin_bytes = 0;
  std::memset(s_skin_sets, 0, sizeof(s_skin_sets));
}

}  // namespace theme_res
