// features/clock.hpp — 時計 Feature。1秒ごとの ClockTick を出すだけ。
#pragma once

#include "watch/feature.hpp"

namespace watch {
namespace features {

extern const FeatureDescriptor kClock;

// 直近に観測した壁時計 (epoch 秒)。UI が初期表示に使う。
int64_t clock_last_epoch_s();

}  // namespace features
}  // namespace watch
