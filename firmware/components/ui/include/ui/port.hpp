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

// ボタン/行タップのクリック音 (audio.click 設定が ON なら鳴る)。
// 音が出ない環境では何もしない。
void click();

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

// ---- テーマ資産 (docs/theme-format.md) ----
// いずれも LVGL コンテキスト内 (port::lock 下 or LVGL タスク) で呼ぶ。

// テーマ資産ディレクトリのルート (末尾 / 無し)。
// firmware: "/assets/themes" (littlefs)、sim: "sim/themes" (CWD=repo root)。
const char* theme_assets_root();

// path を丸ごと読み、テーマ用アリーナに確保する。
// 成功: *out=バッファ (アリーナ内), *out_len=バイト数。失敗: false。
bool theme_asset_load(const char* path, const uint8_t** out,
                      uint32_t* out_len);

// theme_asset_load で確保した全バッファを解放する
// (テーマ適用の直前に1度呼ぶ。アリーナ自体は初期化時に1度だけ確保)。
void theme_assets_reset();

// ---- ファーム更新 (docs/protocol-v1.md) ----
// stage: 0 idle / 1 wifi / 2 download / 3 verify / 4 done / 5 reboot / 6 fail
struct OtaView {
  int stage = 0;
  int pct = 0;  // 0-100
  char msg[40] = {};      // 失敗理由など (短い ASCII)
  char version[24] = {};  // 更新先バージョン
};
// 現在の OTA セッションのスナップショットを out に書く。
void ota_status(OtaView* out);
// 起動中ファームのバージョン (常に非null)。設定/更新画面用。
const char* fw_version();

}  // namespace ui::port
