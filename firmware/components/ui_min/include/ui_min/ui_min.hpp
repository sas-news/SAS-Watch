// 最小 UI: 黒背景に時刻・日付・電池%と、画面下のボタンイベント表示。
// LVGL 操作はすべて bsp_display_lock() 内で行う (docs/plan.md B 章ルール)。
#pragma once

#include "lvgl.h"

namespace ui_min {

// bsp_display_start() で LVGL 起動 -> 画面作成 -> 1秒更新の lv_timer。
// 戻り値は BSP の display handle (失敗時 nullptr)。
lv_display_t* init();

// 画面下に短い表示を出す (ボタンイベント用)。内部で lock を取る。
void notify(const char* text);

// 電池表示を更新する値をセット (内部で lock)。percent < 0 で「--%」。
void set_battery(int percent, bool charging);

// 簡易スクリーンオフ: 明るさ0 (+ 見た目だけ黒画面)。
// 本当の display off (DCS 0x28 / ALDO2 カット) は BSP にパネル公開APIが無い
// TODO(hw): 実機で消費電流を見て DCS off / panel_power 切替を検討
void set_screen_on(bool on);
bool is_screen_on();

// 明るさ (0-100) の設定・取得。screen off から復帰するときに使う。
void set_brightness(int percent);
int  get_brightness();

// タッチ等のアクティビティがあったときに呼ばれるコールバックを登録
// (LVGL タスクのイベント経由で呼ばれる。重い処理はしないこと)
void set_on_activity(void (*cb)(void));

}  // namespace ui_min
