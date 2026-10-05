// theme.hpp — Theme Token。色・角丸・余白・フォントを1か所に集める。
// docs/theme-format.md: 内蔵 (standard/light) か littlefs 上の
// /themes/<id>/ を読んで切替える。画像スロットは無ければ nullptr。
#pragma once

#include "lvgl.h"

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

// theme id を適用 (theme_manager.cpp)。
//   内蔵 → コピー。file → port::theme_asset_* で読んで構築。
//   失敗時は standard にフォールバックして false。
// LVGL コンテキスト内 (LVGL タスクか port::lock 下) で呼ぶこと。
bool theme_apply(const char* id);

// [内部] 現行テーマを差し替える。theme_manager.cpp 専用。
void theme_replace_active(const Theme& t);

}  // namespace ui
