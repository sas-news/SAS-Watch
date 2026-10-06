// features/timer.hpp — 終了時刻ベースの Timer (plan.md E章)。
// スリープでずれないよう、残り時間ではなく終了時刻を持つ。
#pragma once

#include "watch/feature.hpp"

namespace watch {
namespace features {

struct TimerState {
  bool running = false;
  int64_t end_ms = 0;        // monotonic の終了時刻
  uint32_t duration_s = 60;  // 設定された長さ (秒)
  bool finished = false;     // 終了をまだ誰も見ていない
  bool paused = false;       // 一時停止中
  int64_t paused_ms = 0;     // 一時停止時点の残り
};

extern const FeatureDescriptor kTimer;

// UI の Presenter が使う const アクセサ。
const TimerState& timer_state();
// 残りミリ秒 (0 以下にならない)。running なら end - now。
int64_t timer_remaining_ms(int64_t now_ms);

// 内部状態を初期値に戻す (主にテストと工場リセット用)。
void timer_reset_state();

}  // namespace features
}  // namespace watch
