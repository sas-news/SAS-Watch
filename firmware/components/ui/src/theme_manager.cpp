// theme_manager.cpp — theme id → Theme の適用。
//   内蔵 ("standard"/"light") はコピー、それ以外は
//   port::theme_assets_root() 下の <id>/manifest.cbor + .bin を読む。
//   読めない/壊れたテーマは standard にフォールバックする
//   (設定値は触らない。docs/theme-format.md)。
//   全て LVGL コンテキスト内 (port::lock 下 or LVGL タスク) で呼ぶ。
#include "theme.hpp"

#include <cstdio>
#include <cstring>

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
constexpr uint32_t kManifestMax = 32 * 1024;

// 画像 dsc (data はアリーナ内を指す)。slot 数ぶんの静的領域。
lv_image_dsc_t s_img[watch::kThemeImageSlots];
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

bool load_file_theme(const char* id, Theme* out) {
  char path[96];
  std::snprintf(path, sizeof(path), "%s/%s/manifest.cbor",
                port::theme_assets_root(), id);
  const uint8_t* data = nullptr;
  uint32_t len = 0;
  if (!port::theme_asset_load(path, &data, &len) || len > kManifestMax) {
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
  if (m.font_set & 8) t.font_digits_sm = font_for(m.font_px[3], t.font_digits_sm);

  for (int s = 0; s < watch::kThemeImageSlots; ++s) {
    if (!(m.image_set & (1u << s))) continue;
    std::snprintf(path, sizeof(path), "%s/%s/%s", port::theme_assets_root(),
                  id, m.image[s]);
    const uint8_t* bin = nullptr;
    uint32_t blen = 0;
    if (!port::theme_asset_load(path, &bin, &blen) ||
        !watch::theme_image_check(bin, blen, kMaxDim[s][0], kMaxDim[s][1])) {
      continue;  // そのスロットだけ画像なし (テーマ自体は適用する)
    }
    s_img[s].header.magic = bin[0];
    s_img[s].header.cf = static_cast<lv_color_format_t>(bin[1]);
    s_img[s].header.flags = 0;
    s_img[s].header.w = static_cast<uint16_t>(bin[4] | (bin[5] << 8));
    s_img[s].header.h = static_cast<uint16_t>(bin[6] | (bin[7] << 8));
    s_img[s].header.stride = static_cast<uint16_t>(bin[8] | (bin[9] << 8));
    s_img[s].header.reserved_2 = 0;
    s_img[s].data_size = static_cast<uint32_t>(blen - 12);
    s_img[s].data = bin + 12;
    s_img[s].reserved = nullptr;
    s_img[s].reserved_2 = nullptr;
    *img_slot(&t, s) = &s_img[s];
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
  // アリーナは適用ごとに全リセット (前テーマの画像は消える)。
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

}  // namespace ui
