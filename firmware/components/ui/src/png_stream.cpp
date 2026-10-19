// png_stream.cpp — 行ストリーミング PNG デコーダ (firmware 専用)。
// ESP32-S3 ROM の tinfl (miniz) で IDAT を逐次 inflate し、行が揃うたびに
// unfilter → RGB565(+A8) へ変換して dst に書く。32KB の dict +
// 行バッファ 2 枚 + 展開器状態で ~45KB の作業メモリ (すべて PSRAM)。
#include "png_stream.hpp"

#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "miniz.h"

namespace ui {
namespace {

constexpr const char* kTag = "png_stream";

constexpr uint8_t kPngSig[8] = {137, 'P', 'N', 'G', 13, 10, 26, 10};

uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

// Paeth 予測子 (PNG spec 6.6)。
uint8_t paeth(uint8_t a, uint8_t b, uint8_t c) {
  const int p = static_cast<int>(a) + b - c;
  const int pa = p > a ? p - a : a - p;
  const int pb = p > b ? p - b : b - p;
  const int pc = p > c ? p - c : c - p;
  return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}

// cur[n] を unfilter。bpp はピクセルあたりバイト数 (3 or 4)。
// prev は前行の unfilter 済みデータ (最初は全 0)。
// 戻り値: false = 未知のフィルタ番号。
bool unfilter_row(uint8_t filter, uint8_t* cur, const uint8_t* prev,
                  uint32_t n, uint8_t bpp) {
  switch (filter) {
    case 0:  // None
      return true;
    case 1:  // Sub
      for (uint32_t i = bpp; i < n; ++i) cur[i] += cur[i - bpp];
      return true;
    case 2:  // Up
      for (uint32_t i = 0; i < n; ++i) cur[i] += prev[i];
      return true;
    case 3:  // Average
      for (uint32_t i = 0; i < n; ++i) {
        const uint32_t a = (i >= bpp) ? cur[i - bpp] : 0;
        cur[i] += static_cast<uint8_t>((a + prev[i]) >> 1);
      }
      return true;
    case 4:  // Paeth
      for (uint32_t i = 0; i < n; ++i) {
        const uint8_t a = (i >= bpp) ? cur[i - bpp] : 0;
        const uint8_t c = (i >= bpp) ? prev[i - bpp] : 0;
        cur[i] += paeth(a, prev[i], c);
      }
      return true;
    default:
      return false;
  }
}

}  // namespace

uint32_t png_stream_decode(const uint8_t* png, uint32_t len, uint16_t max_w,
                           uint16_t max_h, uint8_t* dst, uint32_t cap,
                           lv_image_dsc_t* dsc) {
  if (!png || !dst || !dsc || len < 33) return 0;
  // シグネチャ + 先頭チャンクが IHDR であること。
  if (std::memcmp(png, kPngSig, 8) != 0 || be32(png + 8) != 13 ||
      std::memcmp(png + 12, "IHDR", 4) != 0) {
    return 0;
  }
  const uint32_t w = be32(png + 16);
  const uint32_t h = be32(png + 20);
  const uint8_t depth = png[24];
  const uint8_t ct = png[25];
  const uint8_t interlace = png[28];
  if (depth != 8 || interlace != 0 || (ct != 2 && ct != 6) ||
      w == 0 || h == 0 || w > max_w || h > max_h) {
    return 0;
  }
  const uint8_t bpp = (ct == 6) ? 4 : 3;
  const uint32_t px = w * h;
  const uint32_t row_bytes = w * bpp;      // 画素部のみ
  const uint32_t row_len = 1 + row_bytes;  // + フィルタバイト
  // 途中までは A8 配置 (不透明確定は末尾) なので px*3+12 必要。
  if (cap < 12u + px * 3u || px * 3u > 0xFFFFFFF0u) return 0;

  // 作業領域を 1 塊で確保: 展開器 + 32KB dict + 現行 + 前行。
  const uint32_t work_sz = sizeof(tinfl_decompressor) + TINFL_LZ_DICT_SIZE +
                           row_len + row_bytes;
  uint8_t* work =
      static_cast<uint8_t*>(heap_caps_malloc(work_sz, MALLOC_CAP_SPIRAM));
  if (!work) {
    ESP_LOGW(kTag, "workbuf alloc failed (%lu)", (unsigned long)work_sz);
    return 0;
  }
  tinfl_decompressor* decomp = reinterpret_cast<tinfl_decompressor*>(work);
  uint8_t* dict = work + sizeof(tinfl_decompressor);
  uint8_t* rowf = dict + TINFL_LZ_DICT_SIZE;
  uint8_t* prev = rowf + row_len;
  std::memset(prev, 0, row_bytes);
  tinfl_init(decomp);

  uint8_t* cplane = dst + 12;
  uint8_t* aplane = cplane + px * 2;
  bool all_opaque = true;
  uint32_t rows_done = 0;
  uint32_t row_fill = 0;
  uint32_t dict_off = 0;
  bool failed = false;
  bool done = false;

  // チャンクを辿り、IDAT のペイロードをそのまま tinfl へ流す。
  uint32_t pos = 8;  // 先頭チャンク (IHDR) の len 位置
  while (!failed && !done && pos + 12 <= len) {
    const uint32_t clen = be32(png + pos);
    const uint8_t* cdata = png + pos + 8;
    if (clen > len - pos - 12) break;  // データ+CRC が収まらない
    const bool is_idat = std::memcmp(png + pos + 4, "IDAT", 4) == 0;
    const bool is_iend = std::memcmp(png + pos + 4, "IEND", 4) == 0;
    pos += 12 + clen;
    if (is_iend) break;
    if (!is_idat || clen == 0) continue;

    // この IDAT を展開。dict はリング: 末尾まで書いたら先頭へ戻る。
    // HAS_MORE_OUTPUT の間は入力が 0 でも呼び続ける (tinfl はシンボル
    // 途中で止まっているだけで、出力先の空きができれば継続できる)。
    const uint8_t* in_p = cdata;
    uint32_t in_len = clen;
    for (;;) {
      size_t in_b = in_len;
      size_t out_b = TINFL_LZ_DICT_SIZE - dict_off;
      const tinfl_status st = tinfl_decompress(
          decomp, in_p, &in_b, dict, dict + dict_off, &out_b,
          TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_HAS_MORE_INPUT);
      in_p += in_b;
      in_len -= static_cast<uint32_t>(in_b);

      // 産出分を行バッファへ。行が揃ったら unfilter → dst へ。
      const uint8_t* q = dict + dict_off;
      size_t n = out_b;
      dict_off += static_cast<uint32_t>(out_b);
      if (dict_off == TINFL_LZ_DICT_SIZE) dict_off = 0;
      while (n > 0 && rows_done < h) {
        const size_t take =
            (n < row_len - row_fill) ? n : (row_len - row_fill);
        std::memcpy(rowf + row_fill, q, take);
        row_fill += static_cast<uint32_t>(take);
        q += take;
        n -= take;
        if (row_fill == row_len) {
          uint8_t* cur = rowf + 1;
          if (!unfilter_row(rowf[0], cur, prev, row_bytes, bpp)) {
            failed = true;
            break;
          }
          uint8_t* cdst = cplane + rows_done * w * 2;
          uint8_t* adst =
              (ct == 6) ? (aplane + rows_done * w) : nullptr;
          for (uint32_t x = 0; x < w; ++x) {
            const uint8_t r = cur[x * bpp + 0];
            const uint8_t g = cur[x * bpp + 1];
            const uint8_t b = cur[x * bpp + 2];
            const uint16_t v = static_cast<uint16_t>(
                ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
            cdst[x * 2] = static_cast<uint8_t>(v);
            cdst[x * 2 + 1] = static_cast<uint8_t>(v >> 8);
            if (adst) {
              const uint8_t a = cur[x * 4 + 3];
              adst[x] = a;
              if (a != 255) all_opaque = false;
            }
          }
          std::memcpy(prev, cur, row_bytes);
          row_fill = 0;
          ++rows_done;
        }
      }
      if (failed) break;
      if (st == TINFL_STATUS_DONE) {
        done = true;
        break;
      }
      if (st == TINFL_STATUS_NEEDS_MORE_INPUT) {
        if (in_len > 0) {
          // 入力が残っているのに more input = 異常 (tinfl は入力を
          // 使い切ってから要求する)。データ壊れ。
          failed = true;
        }
        break;  // 次の IDAT へ
      }
      if (st < 0) {
        failed = true;
        break;
      }
      if (in_b == 0 && out_b == 0) {  // 進捗なし = 無限ループ回避
        failed = true;
        break;
      }
      // TINFL_STATUS_HAS_MORE_OUTPUT: 同じ残り入力で再呼び出し
      // (入力を使い切っていても内部状態からまだ出力できる)。
    }
  }

  uint32_t ret = 0;
  if (!failed && rows_done == h) {
    // ct2 は常に不透明、ct6 は全 α=255 のとき RGB565 に落とす
    // (A8 配置で書いた α 面は data_size 外として捨てる)。
    const bool opaque = (ct == 2) || all_opaque;
    std::memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf =
        opaque ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_RGB565A8;
    dsc->header.w = static_cast<uint16_t>(w);
    dsc->header.h = static_cast<uint16_t>(h);
    dsc->header.stride = static_cast<uint16_t>(w * 2);
    dsc->data_size = px * (opaque ? 2u : 3u);
    dsc->data = dst + 12;
    ret = 12 + dsc->data_size;
    if (!done) {
      // h 行取れたが inflate が未完了 (末尾欠落?) — 警告のみ。
      ESP_LOGW(kTag, "rows ok but inflate not done (%lux%lu)",
               (unsigned long)w, (unsigned long)h);
    }
  }
  heap_caps_free(work);
  return ret;
}

}  // namespace ui
