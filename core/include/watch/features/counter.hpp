// features/counter.hpp — +/- とリセットのシンプルなカウンタ。
#pragma once

#include "watch/feature.hpp"

namespace watch {
namespace features {

extern const FeatureDescriptor kCounter;

// UI の Presenter が使う const アクセサ。
int32_t counter_value();

// 内部状態を初期値に戻す (主にテストと工場リセット用)。
void counter_reset_state();

}  // namespace features
}  // namespace watch
