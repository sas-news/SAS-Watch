// theme_manager.cpp — theme id → Theme の適用。
//   内蔵 ("standard"/"light") はコピー、それ以外は
//   port::theme_assets_root() 下の <id>.zip / <id>/ の manifest.cbor +
//   画像・フォントを読む (docs/theme-format.md v2)。
//   読めない/壊れたテーマは standard にフォールバックする
//   (設定値は触らない)。
//   全て LVGL コンテキスト内 (port::lock 下 or LVGL タスク) で呼ぶ。
#include "theme.hpp"

#include <cstdio>
#include <cstring>

#include "theme_res.hpp"
#include "ui/port.hpp"
#include "watch/theme/manifest.hpp"
#include "watch/theme/package.hpp"

LV_FONT_DECLARE(font_jp_20);
LV_FONT_DECLARE(font_jp_26);
LV_FONT_DECLARE(font_digits_96);
LV_FONT_DECLARE(font_digits_56);

namespace ui {
namespace {

// スロットごとの画像サイズ上限 (docs/theme-format.md)。
constexpr uint16_t kMaxDim[watch::kThemeImageSlots][2] = {
    {410, 502},  // home_bg
    {240, 360},  // stand
    {410, 320},  // timer_done
    {240, 410},  // face_chara
};
constexpr uint16_t kIconMaxDim = 64;   // icons/*.png|bin の上限
constexpr uint16_t kMascotMaxDim = 160;  // mascot expr の上限
constexpr uint32_t kManifestMax = 32 * 1024;

// manifest "bubble" で上書きした文言の保持領域。
char s_bubble[5][48];

const lv_font_t* font_for(uint8_t px, const lv_font_t* fallback) {
  switch (px) {
    case 20: return &font_jp_20;
    case 26: return &font_jp_26;
    case 56: return &font_digits_56;
    case 96: return &font_digits_96;
    default: return fallback;
  }
}

// Theme 内の色フィールドを順序通りに指す表 (manifest color[] と同順)。
lv_color_t* color_at(Theme* t, int i) {
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

const lv_image_dsc_t** img_slot(Theme* t, int i) {
  switch (i) {
    case 0: return &t->img_home_bg;
    case 1: return &t->img_stand;
    case 2: return &t->img_timer_done;
    case 3: return &t->img_face_chara;
    default: return nullptr;
  }
}

// fonts のフォントスロット (kThemeFontKeys 順) → Theme のフォント欄。
const lv_font_t** font_slot(Theme* t, int i) {
  switch (i) {
    case 0: return &t->font_body;
    case 1: return &t->font_title;
    case 2: return &t->font_digits;
    case 3: return &t->font_digits_sm;
    default: return nullptr;
  }
}

bool load_file_theme(const char* id, Theme* out) {
  const uint8_t* data = nullptr;
  uint32_t len = 0;
  if (!port::theme_asset_load(id, "manifest.cbor", &data, &len) ||
      len > kManifestMax) {
    return false;
  }
  watch::ThemeManifest m;
  if (watch::theme_manifest_parse(data, len, &m) !=
          watch::ThemeManifestError::kOk ||
      std::strcmp(m.id, id) != 0) {
    return false;
  }

  // 欠けた Token は standard で補完する。
  Theme t = *builtin_theme("standard");
  for (int i = 0; i < 16; ++i) {
    if (m.color_set & (1u << i)) {
      *color_at(&t, i) = lv_color_hex(m.color[i]);
    }
  }
  if (m.metric_set & 1) t.radius_sm = m.radius_sm;
  if (m.metric_set & 2) t.radius_lg = m.radius_lg;
  if (m.metric_set & 4) t.space = m.space;
  if (m.metric_set & 8) t.anim_ms = m.anim_ms;
  if (m.font_set & 1) t.font_body = font_for(m.font_px[0], t.font_body);
  if (m.font_set & 2) t.font_title = font_for(m.font_px[1], t.font_title);
  if (m.font_set & 4) t.font_digits = font_for(m.font_px[2], t.font_digits);
  if (m.font_set & 8) t.font_digits_sm =
      font_for(m.font_px[3], t.font_digits_sm);

  // ---- v2: fonts (lv_binfont .bin) は font_px より優先 ----
  for (int i = 0; i < watch::kThemeFontSlotCount; ++i) {
    if (!(m.font_file_set & (1u << i))) continue;
    const lv_font_t* f = theme_res::font_load(id, m.font_file[i]);
    if (f) *font_slot(&t, i) = f;  // 失敗したスロットだけ内蔵のまま
  }

  // ---- v1 画像スロット (.bin/.png) ----
  for (int s = 0; s < watch::kThemeImageSlots; ++s) {
    if (!(m.image_set & (1u << s))) continue;
    const lv_image_dsc_t* d =
        theme_res::img_load(id, m.image[s], kMaxDim[s][0], kMaxDim[s][1]);
    if (d) *img_slot(&t, s) = d;  // そのスロットだけ画像なしでも適用
  }

  // ---- v2: screens / style ----
  t.screens_set = m.screens_set;
  std::memcpy(t.screens, m.screens, sizeof(t.screens));
  t.wildcard_skin = m.wildcard_skin;
  t.style = m.style;

  // ---- v2: icons ----
  t.icon_count = m.icon_count;
  std::memcpy(t.icons, m.icons, sizeof(t.icons));
  for (int i = 0; i < m.icon_count; ++i) {
    t.icon_dsc[i] =
        theme_res::img_load(id, m.icons[i].file, kIconMaxDim, kIconMaxDim);
  }

  // ---- v2: mascot ----
  t.mascot = m.mascot;
  for (int i = 0; i < m.mascot.expr_count; ++i) {
    t.mascot_dsc[i] = theme_res::img_load(id, m.mascot.expr_file[i],
                                        kMascotMaxDim, kMascotMaxDim);
  }
  if (t.mascot.expr_count && !t.mascot_dsc[0]) {
    t.mascot.used = 0;  // 先頭の表情が読めなければマスコット自体を無効
  }

  // ---- v2: face_layout ----
  t.face_layout_set = m.face_layout_set;
  std::memcpy(t.face_elem, m.face_elem, sizeof(t.face_elem));
  if ((m.face_layout_set & (1u << watch::kThemeFaceElemChara)) &&
      m.face_elem[watch::kThemeFaceElemChara].img[0]) {
    t.face_chara_dsc =
        theme_res::img_load(id, m.face_elem[watch::kThemeFaceElemChara].img,
                            410, 502);
  }

  // ふきだし文言は manifest 指定分だけ差し替え (残りは standard の既定)。
  for (int i = 0; i < 5; ++i) {
    if (!(m.bubble_set & (1u << i))) continue;
    std::strncpy(s_bubble[i], m.bubble[i], sizeof(s_bubble[i]) - 1);
    s_bubble[i][sizeof(s_bubble[i]) - 1] = '\0';
    t.bubble[i] = s_bubble[i];
  }
  *out = t;
  theme_set_info(m.id, m.name);
  return true;
}

}  // namespace

bool theme_apply(const char* id) {
  // ロード済みフォントを解放してからアリーナを全リセット
  // (binfont がアリーナ内バッファを参照しているため順序厳守)。
  theme_res::reset();
  port::theme_assets_reset();
  const Theme* b = builtin_theme(id);
  Theme t;
  bool ok;
  if (b) {
    t = *b;
    ok = true;
    theme_set_info(id, std::strcmp(id, "standard") == 0 ? "標準" : id);
  } else {
    ok = watch::theme_id_ok(id) && load_file_theme(id, &t);
    // file テーマは load_file_theme 内で manifest の name を記録済み。
  }
  if (!ok) {
    t = *builtin_theme("standard");
    theme_set_info("standard", "標準");
  }
  theme_replace_active(t);
  return ok;
}

// ---- v2 スキン参照 ----

const watch::ThemeScreenSkin* theme_screen_skin(int screen_index) {
  const Theme& t = theme();
  if (screen_index >= 0 && screen_index < watch::kThemeScreenCount &&
      (t.screens_set & (1u << screen_index))) {
    return &t.screens[screen_index];
  }
  if (t.screens_set & (1u << watch::kThemeScreenWildcard)) {
    return &t.wildcard_skin;
  }
  return nullptr;
}

const lv_image_dsc_t* theme_icon(const char* app_id) {
  if (!app_id) return nullptr;
  const Theme& t = theme();
  for (int i = 0; i < t.icon_count; ++i) {
    if (std::strcmp(t.icons[i].app, app_id) == 0) return t.icon_dsc[i];
  }
  return nullptr;
}

bool theme_mascot_on(int screen_index) {
  const Theme& t = theme();
  if (!t.mascot.used || screen_index < 0 ||
      screen_index >= watch::kThemeScreenCount) {
    return false;
  }
  return t.mascot.screens & (1u << screen_index);
}

}  // namespace ui
