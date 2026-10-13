// ui/media_art — BULK "asset" で受け取ったアルバムアートの保持。
// バッファは ble_glue が PSRAM に確保したまま所有し、ここは参照だけ持つ
// (flash には書かない。切断で破棄される)。
#pragma once

#include "lvgl.h"
#include <cstdint>

namespace ui {

// 受信完了の画像を登録する (buf=nullptr で破棄)。
void media_art_set(const uint8_t* rgb565, uint32_t w, uint32_t h,
                   uint32_t art_id);
// art_id が一致する時だけ画像を返す (表示用)。
const lv_image_dsc_t* media_art(uint32_t art_id);

}  // namespace ui
