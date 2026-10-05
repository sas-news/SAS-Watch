// components.hpp — Theme から作る共通部品。
// 全てのインタラクティブ要素に EVENT_BUBBLE を付ける
// (画面ルートまで GESTURE / LONG_PRESSED が届くようにするため)。
#pragma once

#include "theme.hpp"
#include "ui/ui.hpp"

namespace ui::c {

// 画面直上のヘッダ。back_btn=true で左に「戻る」。
lv_obj_t* header(lv_obj_t* scr, const char* title, bool back_btn);

// 縦積み・スクロール可のコンテンツ列。ヘッダの下に敷く。
lv_obj_t* content(lv_obj_t* scr);

// フル幅ボタン (最小 56px)。cb は LV_EVENT_CLICKED。
lv_obj_t* button(lv_obj_t* parent, const char* text, lv_event_cb_t cb,
                 void* user_data);
// プライマリ色のボタン。
lv_obj_t* button_primary(lv_obj_t* parent, const char* text, lv_event_cb_t cb,
                        void* user_data);
// 危険色 (削除など)。
lv_obj_t* button_danger(lv_obj_t* parent, const char* text, lv_event_cb_t cb,
                       void* user_data);

// リスト行: 本文 + 右寄せサブ。タップで cb。
lv_obj_t* list_row(lv_obj_t* parent, const char* text, const char* sub,
                   lv_event_cb_t cb, void* user_data);

// カード (角丸大きめのパネル)。子は縦積み。
lv_obj_t* card(lv_obj_t* parent);

// ラベル + スライダーの行。VALUE_CHANGED で cb。
lv_obj_t* slider_row(lv_obj_t* parent, const char* label, int32_t min,
                     int32_t max, int32_t value, lv_event_cb_t cb,
                     void* user_data);
// slider_row が返すコンテナから実スライダーを取る。
lv_obj_t* slider_of(lv_obj_t* row);

// 小さい補足行。
lv_obj_t* line(lv_obj_t* parent, const char* text);

// 状態ドット (丸)。接続/未接続など。
lv_obj_t* status_dot(lv_obj_t* parent, lv_color_t col);

// 共通フォーマット
void set_dot(lv_obj_t* dot, lv_color_t col);

}  // namespace ui::c
