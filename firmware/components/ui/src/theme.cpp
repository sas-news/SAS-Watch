#include "theme.hpp"

#include <cstring>

// フォントは tools/gen_fonts.py で生成したサブセット (fonts/*.c)。
LV_FONT_DECLARE(font_jp_20);
LV_FONT_DECLARE(font_jp_26);
LV_FONT_DECLARE(font_digits_96);
LV_FONT_DECLARE(font_digits_56);

namespace ui {
namespace {

// ---- 内蔵テーマ ----

// 既定ダーク (AMOLED 向け。黒背景で画素を消す)。
const Theme kStandard = {
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
    .img_home_bg = nullptr,
    .img_stand = nullptr,
    .img_timer_done = nullptr,
};

// 明るめ (屋外視認用)。背景は濃い目のグレーで AMOLED を活かす。
const Theme kLight = {
    .bg = lv_color_hex(0xE8EBF0),
    .surface = lv_color_hex(0xFFFFFF),
    .surface2 = lv_color_hex(0xD6DCE6),
    .primary = lv_color_hex(0x0F6BC5),
    .on_primary = lv_color_hex(0xFFFFFF),
    .text = lv_color_hex(0x14181F),
    .text_dim = lv_color_hex(0x5A6470),
    .accent = lv_color_hex(0xC77400),
    .danger = lv_color_hex(0xD03030),
    .ok = lv_color_hex(0x0E8A50),
    .font_body = &font_jp_20,
    .font_title = &font_jp_26,
    .font_digits = &font_digits_96,
    .font_digits_sm = &font_digits_56,
    .space = 8,
    .radius_sm = 10,
    .radius_lg = 18,
    .anim_ms = 220,
    .tap_min = 56,
    .img_home_bg = nullptr,
    .img_stand = nullptr,
    .img_timer_done = nullptr,
};

// 現在有効なテーマ。theme_apply() が書き換える (LVGL コンテキスト内のみ)。
Theme s_active = kStandard;
char s_id[32] = "standard";
char s_name[48] = "標準";

}  // namespace

const Theme& theme() { return s_active; }

const char* theme_id() { return s_id; }
const char* theme_name() { return s_name; }

void theme_set_info(const char* id, const char* name) {
  if (!id) return;
  std::strncpy(s_id, id, sizeof(s_id) - 1);
  s_id[sizeof(s_id) - 1] = '\0';
  std::strncpy(s_name, (name && name[0]) ? name : id, sizeof(s_name) - 1);
  s_name[sizeof(s_name) - 1] = '\0';
}

const Theme* builtin_theme(const char* id) {
  if (!id) return nullptr;
  if (std::strcmp(id, "standard") == 0) return &kStandard;
  if (std::strcmp(id, "light") == 0) return &kLight;
  return nullptr;
}

// theme_manager.cpp から現行テーマを差し替える (LVGL コンテキスト内のみ)。
void theme_replace_active(const Theme& t) { s_active = t; }

}  // namespace ui
