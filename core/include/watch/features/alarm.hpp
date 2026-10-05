// features/alarm.hpp — 目覚まし (最大5件、時刻+曜日繰り返し、スヌーズ5分)。
// 時刻はローカル時刻 (tz_offset_min 適用後) の hh:mm で持つ。
// 次回発火は epoch 秒で計算し、firmware が PCF85063 のアラームに書き込む。
// Deep Sleep 中は Runtime::next_deadline_ms 経由の RTC timer wakeup で起床する
// (GPIO39 の RTC INT は RTC GPIO ではなく deep sleep から復帰できない。
//  light sleep では使える → board::sleep::arm_light_sleep_wake)。
#pragma once

#include "watch/feature.hpp"

namespace watch {
namespace features {

constexpr size_t kAlarmMax = 5;
constexpr int64_t kAlarmSnoozeS = 5 * 60;      // スヌーズ 5 分
constexpr int64_t kAlarmRingTimeoutMs = 120'000;  // 鳴動の自動停止 (2分)

struct AlarmEntry {
  uint32_t id = 0;   // 0 = 空スロット
  uint8_t hour = 0;  // 0-23 (ローカル時刻)
  uint8_t min = 0;   // 0-59
  // 曜日マスク: bit0=日 .. bit6=土 (tm_wday 準拠)。0 = 毎日。
  uint8_t dow = 0;
  uint8_t enabled = 0;
};

extern const FeatureDescriptor kAlarm;

size_t alarm_count();
// i = 0 が最古 (id 昇順)。範囲外は false。
bool alarm_at(size_t i, AlarmEntry* out);
bool alarm_find(uint32_t id, AlarmEntry* out);

// 追加/更新。id=0 で新規 (next_id から採番)、既存 id なら上書き。
// 戻り値: 確定した id (>0)。-1 引数不正 / -2 満杯 / -3 指定 id が無い。
int32_t alarm_set(uint32_t id, uint8_t hour, uint8_t min, uint8_t dow,
                  bool enabled, FeatureContext& ctx);
// 1 = 消えた / 0 = 無かった。
int32_t alarm_delete(uint32_t id, FeatureContext& ctx);

// 鳴動中か (「止める/スヌーズ」UI の判定)。
bool alarm_ringing();
uint32_t alarm_ringing_id();

// 次に鳴る予定の epoch 秒 (スヌーズ込み)。なければ 0。
// firmware はこの値を RTC アラームに書き込む。
int64_t alarm_next_fire_epoch();

// 曜日表示用: dow マスクが wday (0=日..6=土) を含むか。0 は毎日扱いで true。
bool alarm_dow_has(uint8_t dow, int wday);

// 内部状態を初期値に戻す (テスト・工場リセット用)。KV は消さない。
void alarm_reset_state();

}  // namespace features
}  // namespace watch
