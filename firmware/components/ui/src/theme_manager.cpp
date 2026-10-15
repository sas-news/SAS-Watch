// theme_manager.cpp — theme id → Theme の適用。
//   内蔵 ("standard"/"light") はコピー、それ以外は
//   port::theme_assets_root() 下の <id>.zip / <id>/ の manifest.cbor +
//   画像・フォントを読む (docs/theme-format.md v2)。
//   読めない/壊れたテーマは standard にフォールバックする
//   (設定値は触らない)。
//   全て LVGL コンテキスト内 (port::lock 下 or LVGL タスク) で呼ぶ。
#include "theme.hpp"

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "esp_log.h"
#define SAS_BIG_STATIC EXT_RAM_BSS_ATTR
#else
#define SAS_BIG_STATIC
#endif

#include <cstdio>
#include <cstring>

#include "theme_res.hpp"
#include "ui/port.hpp"
#include "watch/theme/manifest.hpp"
#include "watch/theme/package.hpp"

LV_FONT_DECLARE(font_jp_20);
LV_FONT_DECLARE(font_jp_26);
LV_FONT_DECLARE(font_jp_32);
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
    case 32: return &font_jp_32;
    case 56: return &font_digits_56;
    case 96: return &font_digits_96;
    default: return fallback;
  }
}

// Theme 内の色フィールド表は theme.cpp の theme_color_at を使う。

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
  // manifest はアリーナをまだ確保していないので tmp (PSRAM) に読んで
  // parse → 参照資産の必要量を見積もってからアリーナをその分だけ確保。
  // (常時 4MiB 握ると PSRAM の残りが ~40KB になり、描画レイヤーや
  // fopen のロック確保が内蔵 RAM に溢れて abort する。cute で実機観測)
  uint32_t mlen = 0;
  if (!port::theme_entry_stat(id, "manifest.cbor", &mlen) || mlen == 0 ||
      mlen > kManifestMax) {
    return false;
  }
  uint8_t* mtmp = port::theme_tmp_alloc(mlen);
  if (!mtmp) return false;
  bool ok = port::theme_entry_read(id, "manifest.cbor", mtmp, mlen);
  // LVGL コンテキスト内のみから呼ばれるので static でスタックを節約。
  // ThemeManifest は約 11.7KB。スタックに置かない。
  SAS_BIG_STATIC static watch::ThemeManifest m;
  if (ok) {
    ok = watch::theme_manifest_parse(mtmp, mlen, &m) ==
             watch::ThemeManifestError::kOk &&
         std::strcmp(m.id, id) == 0;
  }
  port::theme_tmp_free(mtmp);
  if (!ok) return false;

  port::theme_assets_reset(theme_res::assets_estimate(id, &m));

  // 欠けた Token は standard で補完する。
  // Theme は ~5KB あり main タスクスタック(3584B)に乗らないため、
  // LVGL コンテキスト内のみから呼ばれる前提で static で持つ。
  SAS_BIG_STATIC static Theme t;
  t = *builtin_theme("standard");
  for (int i = 0; i < 19; ++i) {
    if (m.color_set & (1u << i)) {
      *theme_color_at(&t, i) = lv_color_hex(m.color[i]);
    }
  }
  if (m.metric_set & 1) t.radius_sm = m.radius_sm;
  if (m.metric_set & 2) t.radius_lg = m.radius_lg;
  if (m.metric_set & 4) t.space = m.space;
  if (m.metric_set & 8) t.anim_ms = LV_MIN(m.anim_ms, 150);
  if (m.font_set & 1) t.font_body = font_for(m.font_px[0], t.font_body);
  if (m.font_set & 2) {
    t.font_title = font_for(m.font_px[1], t.font_title);
    t.font_row = t.font_title;  // 行の主テキストはテーマのタイトル系に合わせる
  }
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

  // ---- v5: assets (id→file)。dsc は遅延ロード (theme_image) ----
  t.asset_count = m.asset_count;
  std::memcpy(t.assets, m.assets, sizeof(t.assets));
  for (int i = 0; i < watch::kThemeMaxAssets; ++i) t.asset_dsc[i] = nullptr;

  // ---- v3: skin (9-slice 画像スキン) ----
  t.skin_set = m.skin_set;
  std::memcpy(t.skin_part, m.skin_part, sizeof(t.skin_part));
  for (int i = 0; i < watch::kThemeSkinPartCount; ++i) {
    if (!(m.skin_set & (1u << i))) continue;
    t.skin[i] = theme_res::skin_load(id, m.skin_part[i]);
    // 読めない part は nullptr のまま → ベクタ描画フォールバック。
#ifndef ESP_PLATFORM
    std::fprintf(stderr, "skin[%s] %s -> %s\n", watch::kThemeSkinPartNames[i],
                 m.skin_part[i].img, t.skin[i] ? "ok" : "NULL");
#endif
  }
  t.skin_bytes = theme_res::skin_bytes();

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
  const Theme* b = builtin_theme(id);
  // sizeof(Theme) ~5KB > main タスクスタック(3584B) → static。
  SAS_BIG_STATIC static Theme t;
  bool ok;
  if (b) {
    // 内蔵テーマは資産を使わない → アリーナは画面ブロック分だけに縮める。
    port::theme_assets_reset(0);
    t = *b;
    ok = true;
    theme_set_info(id, std::strcmp(id, "standard") == 0 ? "標準" : id);
  } else {
    // file テーマは load_file_theme 内で見積もり→reset(need) する。
    ok = watch::theme_id_ok(id) && load_file_theme(id, &t);
    // file テーマは load_file_theme 内で manifest の name を記録済み。
  }
  if (!ok) {
    t = *builtin_theme("standard");
    theme_set_info("standard", "標準");
  }
  theme_replace_active(t);
#if defined(ESP_PLATFORM)
  ESP_LOGI("theme", "apply '%s' -> %s", id ? id : "(null)",
           ok ? theme_id() : "standard(fallback)");
#endif
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

const lv_image_dsc_t* theme_image(const char* asset_id) {
  if (!asset_id) return nullptr;
  Theme& t = const_cast<Theme&>(theme());
  for (int i = 0; i < t.asset_count; ++i) {
    if (std::strcmp(t.assets[i].id, asset_id) == 0) {
      if (!t.asset_dsc[i]) {
        // 上限は LVGL の描画サイズに合わせて広め (装飾用途)。
        t.asset_dsc[i] =
            theme_res::img_load(theme_id(), t.assets[i].file, 410, 502);
      }
      return t.asset_dsc[i];
    }
  }
  return nullptr;
}

const lv_image_dsc_t* theme_icon(const char* app_id) {
  if (!app_id) return nullptr;
  const Theme& t = theme();
  for (int i = 0; i < t.icon_count; ++i) {
    if (std::strcmp(t.icons[i].app, app_id) == 0) return t.icon_dsc[i];
  }
  // v5: assets の "icon.<app>" も引く (icons キー省略でも可)
  char aid[48];
  const int n = std::snprintf(aid, sizeof(aid), "icon.%s", app_id);
  if (n > 0 && n < static_cast<int>(sizeof(aid))) {
    return theme_image(aid);
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

const theme_res::SkinSet* theme_skin(watch::ThemeSkinPartId part) {
  if (part < 0 || part >= watch::kThemeSkinPartCount) return nullptr;
  return theme().skin[part];
}

uint32_t theme_skin_bytes() { return theme().skin_bytes; }

}  // namespace ui
