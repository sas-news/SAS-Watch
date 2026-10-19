// png_stream.hpp — 行ストリーミング PNG デコーダ (firmware 専用)。
// lodepng は「展開バッファ全体 (~820KB) + ARGB 出力 (~823KB)」を同時に
// 確保するため、全面描画バッファ化後の PSRAM では全面背景が確保できない。
// こちらは ROM tinfl で行単位に展開するので作業メモリは ~50KB で済む。
#pragma once

#include <cstdint>

#include "lvgl.h"

namespace ui {

// 8bit・非インターレースの PNG (color type 2=RGB / 6=RGBA) を行単位で展開し、
// theme_res.cpp の png_to_lv() と同じ出力を作る:
//   - 全ピクセル不透明 → RGB565 (dst+12 から色面のみ、px*2 バイト)
//   - 透過あり          → RGB565A8 (色面 + α面、px*3 バイト)
// 戻り値: dst に書いた総バイト数 (12 + data_size)。0 = 失敗
// (対応外の形式・壊れたデータ・cap 不足)。失敗時は呼び出し側が
// lodepng にフォールバックする。
// dst[cap] には少なくとも 12 + w*h*3 が必要 (途中まで RGB565A8 配置で書く)。
uint32_t png_stream_decode(const uint8_t* png, uint32_t len, uint16_t max_w,
                           uint16_t max_h, uint8_t* dst, uint32_t cap,
                           lv_image_dsc_t* dsc);

}  // namespace ui
