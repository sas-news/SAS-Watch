// theme.hpp — Theme Token。色・角丸・余白・フォントを1か所に集める。
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
};

const Theme& theme();

}  // namespace ui
