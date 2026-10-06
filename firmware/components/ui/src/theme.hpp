// theme.hpp — Theme Token。色・角丸・余白・フォントを1か所に集める。
// docs/theme-format.md: 内蔵 (standard/light) か littlefs 上の
// /themes/<id>/ を読んで切替える。画像スロットは無ければ nullptr。
#pragma once

#include "lvgl.h"
#include "watch/theme/manifest.hpp"

namespace ui {

struct Theme {
  lv_color_t bg;          // 画面背景 (AMOLED なので黒)
  lv_color_t surface;     // カード面
  lv_color_t surface2;    // 押せる面 (ボタン・行)
  lv_color_t primary;     // 強調 (開始・有効)
  lv_color_t on_primary;  // primary 上の文字色
  lv_color_t text;        // 本文
  lv_color_t text_dim;    // 補足
  lv_color_t accent;      // 注意・数値ハイライト
  lv_color_t danger;      // 削除・警告
  lv_color_t ok;          // 正常・接続中
  lv_color_t accent2;     // 第2アクセント (ウォーム系: analog の針・数字など)
  lv_color_t accent3;     // 第3アクセント (ネオン系: HUD 飾り・バーなど)
  lv_color_t accent4;     // 第4アクセント (バイオレット系: グロー・進捗など)
  lv_color_t accent5;     // 第5アクセント (ピンク系: キャラ文字盤の強調)
  lv_color_t bubble_bg;   // chara_bubble ふきだしの背景
  lv_color_t bubble_text; // chara_bubble ふきだしの文字色
  lv_color_t line;        // リスト区切り線・スライダーのトラック
  lv_color_t primary2;    // primary のグラデーション終端 (スライダー塗り)
  lv_color_t edge;        // 行末 chevron・スイッチ OFF トラックの中間色

  const lv_font_t* font_body;     // 本文 (日本語 20px)
  const lv_font_t* font_title;    // ヘッダ/大きめ本文 (日本語 26px)
  const lv_font_t* font_digits;   // 時刻など特大数字 96px
  const lv_font_t* font_digits_sm;// タイマー等の大きい数字 56px

  uint8_t space;      // 基本余白 (=8)
  uint8_t radius_sm;  // 小さい角丸 (=10)
  uint8_t radius_lg;  // カード角丸 (=18)
  uint16_t anim_ms;   // 画面遷移 (=220)
  uint8_t tap_min;    // タップ最小高さ (=56)

  // 画像スロット (docs/theme-format.md)。nullptr = 画像なし。
  const lv_image_dsc_t* img_home_bg;    // Home 背景 (下帯)
  const lv_image_dsc_t* img_stand;      // Home 立ち絵
  const lv_image_dsc_t* img_timer_done; // タイマー終了アラート
  const lv_image_dsc_t* img_face_chara; // 文字盤 chara_* 用立ち絵 (透過 ≤240x410)

  // chara_bubble 文字盤のふきだし文言 (manifest "bubble" で上書き可)。
  // 0:朝 1:昼 2:夕 3:夜 4:歩数目標の残り ({n}=残り歩数)。
  const char* bubble[5];

  // ---- v2 スキン (file テーマのみ有効。内蔵テーマはゼロ初期化) ----
  watch::ThemeStyleSkin style;  // 有効ビットは style.set
  // 画面別スキン (screens_set の bit が有効フラグ、wildcard は "*")。
  uint32_t screens_set = 0;
  watch::ThemeScreenSkin screens[watch::kThemeScreenCount] = {};
  watch::ThemeScreenSkin wildcard_skin = {};
  // アプリアイコン (icons[i].app ↔ icon_dsc[i]。nullptr=タイル表示)。
  watch::ThemeIcon icons[watch::kThemeMaxIcons] = {};
  const lv_image_dsc_t* icon_dsc[watch::kThemeMaxIcons] = {};
  uint8_t icon_count = 0;
  // マスコット (used + expr 画像)。
  watch::ThemeMascot mascot = {};
  const lv_image_dsc_t* mascot_dsc[watch::kThemeMascotExprMax] = {};
  // face_layout (face "theme" が読む)。
  watch::ThemeFaceElem face_elem[watch::kThemeFaceElemCount] = {};
  uint32_t face_layout_set = 0;
  // face_layout で指定した画像の dsc (chara 用)。
  const lv_image_dsc_t* face_chara_dsc = nullptr;
};

// 現在有効な Theme (適用済みスナップショット)。
const Theme& theme();

// 適用中テーマの id / 表示名 (file テーマは manifest の name、無ければ id)。
const char* theme_id();
const char* theme_name();

// [内部] 適用中テーマの id/name を記録する。theme_manager.cpp 専用。
void theme_set_info(const char* id, const char* name);

// 内蔵テーマ ("standard" / "light")。無ければ nullptr。
const Theme* builtin_theme(const char* id);

// ---- v2 スキン参照ヘルパ ----
// 画面インデックス (kThemeScreenNames 順) → 有効スキン。
// 個別指定が無ければ "*" フォールバック。両方無ければ nullptr。
const watch::ThemeScreenSkin* theme_screen_skin(int screen_index);
// アプリ id → アイコン画像 (無ければ nullptr → 文字タイル)。
const lv_image_dsc_t* theme_icon(const char* app_id);
// マスコットがその画面で有効か (manifest.screens マスク)。
bool theme_mascot_on(int screen_index);

// theme id を適用 (theme_manager.cpp)。
//   内蔵 → コピー。file → port::theme_asset_* で読んで構築。
//   失敗時は standard にフォールバックして false。
// LVGL コンテキスト内 (LVGL タスクか port::lock 下) で呼ぶこと。
bool theme_apply(const char* id);

// [内部] 現行テーマを差し替える。theme_manager.cpp 専用。
void theme_replace_active(const Theme& t);

}  // namespace ui
