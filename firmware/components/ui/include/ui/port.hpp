// port.hpp — ui が platform に要求する最小インターフェース。
// firmware 側は esp_port.cpp、sim 側は sim_port.cpp が実装する。
#pragma once

#include <cstddef>
#include <cstdint>

namespace ui::port {

// LVGL タスクと排他するロック (firmware: lvgl_port_lock)。
// 返り値 false = タイムアウト (その async 経路は落とされる)。
bool lock(uint32_t timeout_ms);
void unlock();

// Event リング用のごく短いクリティカルセクション
// (firmware: portMUX、sim: no-op — シングルスレッド)。
void crit_enter();
void crit_exit();

// monotonic ミリ秒 (core の Clock と同じ時計でなければならない)。
int64_t now_ms();

// 電池残量 0-100。読めなければ -1。
int battery_percent();
bool battery_charging();

// バイブ。振動なしの環境では何もしない。
void vibrate(uint32_t ms);

// デバイス再起動 (power menu)。
[[noreturn]] void restart();

// 端末情報を 1行文字列で返す (settings 画面用)。
// 例 "ESP32-S3 / SAS-Watch dev / FW 0.1"。書いた文字数を返す。
size_t device_info(char* buf, size_t cap);

// PWR 長押しでハード電源OFFする秒数 (board::kPowerOffHoldSeconds と同じ)。
int power_off_hold_seconds();

// パネルの display on/off (ScreenOff 適用層が使う)。
void display_power(bool on);

// バックライト輝度 0-100 (Dim/ScreenOff/復帰で使う)。
void brightness_apply(int percent);

}  // namespace ui::port
