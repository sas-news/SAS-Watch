// features/steps.hpp — 歩数計。ImuSample (accel) を簡易ピーク検出で数え、
// 日付が変わったら 0 に戻して当日分だけ KV に永続化する。
#pragma once

#include "watch/feature.hpp"

namespace watch {
namespace features {

extern const FeatureDescriptor kSteps;

// UI / BLE が使う const アクセサ。
uint32_t steps_today();

// 内部状態を初期値に戻す (主にテストと工場リセット用)。
void steps_reset_state();

}  // namespace features
}  // namespace watch
