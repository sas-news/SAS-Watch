// features/stopwatch.hpp — ラップ最大20件のストップウォッチ。
#pragma once

#include "watch/feature.hpp"

namespace watch {
namespace features {

constexpr size_t kStopwatchMaxLaps = 20;

struct StopwatchState {
  bool running = false;
  int64_t started_ms = 0;   // 走り始めた monotonic 時刻
  int64_t acc_ms = 0;       // 停止中に積み上がった時間
  size_t lap_count = 0;
  int64_t laps[kStopwatchMaxLaps] = {};  // ラップ時点の通算時間
};

extern const FeatureDescriptor kStopwatch;

// UI の Presenter が使う const アクセサ。
const StopwatchState& stopwatch_state();
// 現在の通算時間 (running なら acc + (now - started))。
int64_t stopwatch_elapsed_ms(int64_t now_ms);

// 内部状態を初期値に戻す (主にテストと工場リセット用)。
void stopwatch_reset_state();

}  // namespace features
}  // namespace watch
