// ビルドに含まれる Feature の一覧 (plan.md E章)。
// 増やすときは descriptor を作ってここに1行足す。
#include "watch/feature.hpp"
#include "watch/features/agent.hpp"
#include "watch/features/clock.hpp"
#include "watch/features/counter.hpp"
#include "watch/features/memo.hpp"
#include "watch/features/steps.hpp"
#include "watch/features/stopwatch.hpp"
#include "watch/features/timer.hpp"

namespace watch {

namespace {
const FeatureDescriptor* const kBuiltin[] = {
    &features::kClock,   &features::kTimer, &features::kStopwatch,
    &features::kCounter, &features::kSteps, &features::kMemo,
    &features::kAgent,
};
}

const FeatureDescriptor* const* builtin_features() { return kBuiltin; }
size_t builtin_features_count() {
  return sizeof(kBuiltin) / sizeof(kBuiltin[0]);
}

}  // namespace watch
