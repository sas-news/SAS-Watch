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

// chara_bubble 文字盤のふきだし既定文言 (manifest "bubble" で上書き可)。
// 0:朝(5-10時) 1:昼(10-16時) 2:夕(16-19時) 3:夜(19-5時) 4:歩数残り
constexpr const char* kBubbleDefault[5] = {
    "おはよう！",
    "こんにちは！",
    "おつかれさま！",
    "おやすみ〜",
    "あと{n}歩だよ",
};

// 既定ダーク (AMOLED 向け。黒背景で画素を消す)。
// accent 系はデザイン見本 (docs/design/faces) の色を標準値にしている。
const Theme kStandard = {
    .bg = lv_color_hex(0x000000),
    .surface = lv_color_hex(0x14161D),
    .surface2 = lv_color_hex(0x1F222C),
    .primary = lv_color_hex(0xFF8A3D),
    .on_primary = lv_color_hex(0x1A0D00),
    .text = lv_color_hex(0xF4F4F6),
    .text_dim = lv_color_hex(0x8A8F9C),
    .accent = lv_color_hex(0x4CC9F0),
    .danger = lv_color_hex(0xFF5C6C),
    .ok = lv_color_hex(0x3DDC97),
    .accent2 = lv_color_hex(0xE8D7B0),
    .accent3 = lv_color_hex(0x00E0C6),
    .accent4 = lv_color_hex(0x7A5CFF),
    .accent5 = lv_color_hex(0xFF5C8A),
    .bubble_bg = lv_color_hex(0xFFFFFF),
    .bubble_text = lv_color_hex(0x1A0F2E),
    .line = lv_color_hex(0x2A2E3A),
    .primary2 = lv_color_hex(0xFFB27A),
    .edge = lv_color_hex(0x4A4F5C),
    .font_body = &font_jp_20,
    .font_title = &font_jp_26,
    .font_digits = &font_digits_96,
    .font_digits_sm = &font_digits_56,
    .space = 8,
    .radius_sm = 10,
    .radius_lg = 22,
    .anim_ms = 220,
    .tap_min = 56,
    .img_home_bg = nullptr,
    .img_stand = nullptr,
    .img_timer_done = nullptr,
    .img_face_chara = nullptr,
    .bubble = {kBubbleDefault[0], kBubbleDefault[1], kBubbleDefault[2],
               kBubbleDefault[3], kBubbleDefault[4]},
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
    .accent2 = lv_color_hex(0x8A6B35),
    .accent3 = lv_color_hex(0x007A6B),
    .accent4 = lv_color_hex(0x5E3FD1),
    .accent5 = lv_color_hex(0xC22A5C),
    .bubble_bg = lv_color_hex(0xFFFFFF),
    .bubble_text = lv_color_hex(0x1A0F2E),
    .line = lv_color_hex(0xC3CAD4),
    .primary2 = lv_color_hex(0x1E86E0),
    .edge = lv_color_hex(0x98A0B0),
    .font_body = &font_jp_20,
    .font_title = &font_jp_26,
    .font_digits = &font_digits_96,
    .font_digits_sm = &font_digits_56,
    .space = 8,
    .radius_sm = 10,
    .radius_lg = 22,
    .anim_ms = 220,
    .tap_min = 56,
    .img_home_bg = nullptr,
    .img_stand = nullptr,
    .img_timer_done = nullptr,
    .img_face_chara = nullptr,
    .bubble = {kBubbleDefault[0], kBubbleDefault[1], kBubbleDefault[2],
               kBubbleDefault[3], kBubbleDefault[4]},
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
