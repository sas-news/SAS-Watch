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
//
// パッケージは <root>/<id>.zip (stored のみ・展開しない) が本命。
// v1 で展開済みの <root>/<id>/<name> はフォールバックとして読む。

// テーマ資産ディレクトリのルート (末尾 / 無し)。
// firmware: "/assets/themes" (littlefs)、sim: "sim/themes" (CWD=repo root)。
const char* theme_assets_root();

// テーマ id 内の 1 エントリのサイズを返す。無ければ false。
bool theme_entry_stat(const char* id, const char* name, uint32_t* out_size);
// dst[cap] にエントリ全体を読む。cap 不足/読み失敗なら false。
bool theme_entry_read(const char* id, const char* name, uint8_t* dst,
                      uint32_t cap);

// エントリをグローバル領域 (アリーナ) に読み込んで確保する。
// 成功: *out=バッファ (アリーナ内), *out_len=バイト数。失敗: false。
// アリーナはテーマ適用ごとに theme_assets_reset() で全解放。
bool theme_asset_load(const char* id, const char* name, const uint8_t** out,
                      uint32_t* out_len);
// アリーナに n バイトだけ確保する (デコード済データの置き場所)。
// 満杯なら nullptr。確保した領域は theme_assets_reset() まで有効。
uint8_t* theme_asset_alloc(uint32_t n);
void theme_assets_reset();

// 画面背景用ブロック (PSRAM アリーナ先頭の固定 2 ブロック)。
// 画面オープン時に theme_screen_block() で1枚取り、画面 DELETE で
// theme_screen_free() に返す。満杯なら nullptr (背景なしで継続)。
constexpr uint32_t kThemeScreenBlockSize = 640 * 1024;
uint8_t* theme_screen_block();
void theme_screen_free(uint8_t* p);

// 一時領域 (PNG デコードの作業バッファなど)。firmware=PSRAM、sim=malloc。
// 使い終わったら必ず theme_tmp_free()。
uint8_t* theme_tmp_alloc(uint32_t n);
void theme_tmp_free(uint8_t* p);

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
