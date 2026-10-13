// media_art — BULK asset で届いたアルバムアート (PSRAM バッファへの参照)。
#include "ui/media_art.hpp"

namespace ui {
namespace {

lv_image_dsc_t s_dsc = {};
uint32_t s_id = 0;

}  // namespace

void media_art_set(const uint8_t* rgb565, uint32_t w, uint32_t h,
                   uint32_t art_id) {
  if (!rgb565 || art_id == 0) {
    s_dsc = lv_image_dsc_t{};
    s_id = 0;
    return;
  }
  s_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
  s_dsc.header.w = static_cast<uint16_t>(w);
  s_dsc.header.h = static_cast<uint16_t>(h);
  s_dsc.header.stride = 0;
  s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  s_dsc.header.flags = 0;
  s_dsc.header.reserved_2 = 0;
  s_dsc.data_size = w * h * 2;
  s_dsc.data = rgb565;
  s_id = art_id;
}

const lv_image_dsc_t* media_art(uint32_t art_id) {
  return (s_dsc.data && s_id == art_id) ? &s_dsc : nullptr;
}

}  // namespace ui
