#include "theme.hpp"

// フォントは tools/gen_fonts.py で生成したサブセット (fonts/*.c)。
LV_FONT_DECLARE(font_jp_20);
LV_FONT_DECLARE(font_jp_26);
LV_FONT_DECLARE(font_digits_96);
LV_FONT_DECLARE(font_digits_56);

namespace ui {

const Theme& theme() {
  static const Theme t = {
      .bg = lv_color_hex(0x000000),
      .surface = lv_color_hex(0x131820),
      .surface2 = lv_color_hex(0x1E2634),
      .primary = lv_color_hex(0x4DA3FF),
      .on_primary = lv_color_hex(0x051018),
      .text = lv_color_hex(0xF2F5F8),
      .text_dim = lv_color_hex(0x8B95A3),
      .accent = lv_color_hex(0xFFC24D),
      .danger = lv_color_hex(0xFF5C5C),
      .ok = lv_color_hex(0x46E29A),
      .font_body = &font_jp_20,
      .font_title = &font_jp_26,
      .font_digits = &font_digits_96,
      .font_digits_sm = &font_digits_56,
      .space = 8,
      .radius_sm = 10,
      .radius_lg = 18,
      .anim_ms = 220,
      .tap_min = 56,
  };
  return t;
}

}  // namespace ui
