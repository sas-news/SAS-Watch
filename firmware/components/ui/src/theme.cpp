#include "theme.hpp"

#include <cstring>

// フォントは tools/gen_fonts.py で生成したサブセット (fonts/*.c)。
// 日本語フォントの実体は assets の fonts/jp_<px>.bin (T24)。
// ここで指すのはフォールバックの最小フォント (ASCII+かな+記号) で、
// theme_apply 時に theme_manager::fix_builtin_fonts が本物に差し替える。
LV_FONT_DECLARE(font_jp_min_20);
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
    .font_body = &font_jp_min_20,
    .font_row = &font_jp_min_20,
    .font_title = &font_jp_min_20,
    .font_digits = &font_digits_96,
    .font_digits_sm = &font_digits_56,
    .space = 8,
    .radius_sm = 10,
    .radius_lg = 22,
    .anim_ms = 0,   // 全面 redraw が遅いので既定は即時切替
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
    .font_body = &font_jp_min_20,
    .font_row = &font_jp_min_20,
    .font_title = &font_jp_min_20,
    .font_digits = &font_digits_96,
    .font_digits_sm = &font_digits_56,
    .space = 8,
    .radius_sm = 10,
    .radius_lg = 22,
    .anim_ms = 0,   // 全面 redraw が遅いので既定は即時切替
    .tap_min = 56,
    .img_home_bg = nullptr,
    .img_stand = nullptr,
    .img_timer_done = nullptr,
    .img_face_chara = nullptr,
    .bubble = {kBubbleDefault[0], kBubbleDefault[1], kBubbleDefault[2],
               kBubbleDefault[3], kBubbleDefault[4]},
};

// 現在有効なテーマ。theme_apply() が書き換える (LVGL コンテキスト内のみ)。
// s_base = テーマ本体、s_active = 画面別 tokens 上書きの適用済み。
Theme s_base = kStandard;
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
void theme_replace_active(const Theme& t) {
  s_base = t;
  s_active = t;
}

// Theme 内の色フィールドを順序通りに指す表 (manifest color[] と同順)。
lv_color_t* theme_color_at(Theme* t, int i) {
  switch (i) {
    case 0: return &t->bg;
    case 1: return &t->surface;
    case 2: return &t->surface2;
    case 3: return &t->primary;
    case 4: return &t->on_primary;
    case 5: return &t->text;
    case 6: return &t->text_dim;
    case 7: return &t->accent;
    case 8: return &t->danger;
    case 9: return &t->ok;
    case 10: return &t->accent2;
    case 11: return &t->accent3;
    case 12: return &t->accent4;
    case 13: return &t->accent5;
    case 14: return &t->bubble_bg;
    case 15: return &t->bubble_text;
    case 16: return &t->line;
    case 17: return &t->primary2;
    case 18: return &t->edge;
    default: return nullptr;
  }
}

// v4: 画面別 tokens 上書き。"*" の tokens → 画面個別の tokens の順に適用。
void theme_apply_screen_tokens(int screen_index) {
  s_active = s_base;  // 前画面の上書きを戻す
  auto apply = [](const watch::ThemeScreenSkin& sk, uint32_t set) {
    for (int i = 0; i < 19 && set; ++i, set >>= 1) {
      if (set & 1u) {
        lv_color_t* p = theme_color_at(&s_active, i);
        if (p) *p = lv_color_hex(sk.token_color[i]);
      }
    }
  };
  if (s_base.screens_set & (1u << watch::kThemeScreenWildcard)) {
    apply(s_base.wildcard_skin, s_base.wildcard_skin.token_set);
  }
  if (screen_index >= 0 && screen_index < watch::kThemeScreenCount &&
      (s_base.screens_set & (1u << screen_index))) {
    apply(s_base.screens[screen_index],
          s_base.screens[screen_index].token_set);
  }
}

}  // namespace ui
