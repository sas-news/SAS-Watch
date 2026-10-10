// ビルドに含まれる Feature の一覧 (plan.md E章)。
// 増やすときは descriptor を作ってここに1行足す。
// アプリ (apps/<id>/) は SAS_APP_<ID> が 1 の時だけ登録される。
#include "watch/feature.hpp"
#if SAS_APP_ALARM
#include "watch/features/alarm.hpp"
#endif
#if SAS_APP_AGENT
#include "watch/features/agent.hpp"
#endif
#include "watch/features/clock.hpp"
#if SAS_APP_COUNTER
#include "watch/features/counter.hpp"
#endif
#if SAS_APP_MEDIA
#include "watch/features/media.hpp"
#endif
#if SAS_APP_MEMO
#include "watch/features/memo.hpp"
#endif
#if SAS_APP_NOTIFY
#include "watch/features/notify.hpp"
#endif
#if SAS_APP_STEPS
#include "watch/features/steps.hpp"
#endif
#if SAS_APP_STOPWATCH
#include "watch/features/stopwatch.hpp"
#endif
#if SAS_APP_TIMER
#include "watch/features/timer.hpp"
#endif

namespace watch {

namespace {
const FeatureDescriptor* const kBuiltin[] = {
    &features::kClock,
#if SAS_APP_TIMER
    &features::kTimer,
#endif
#if SAS_APP_STOPWATCH
    &features::kStopwatch,
#endif
#if SAS_APP_COUNTER
    &features::kCounter,
#endif
#if SAS_APP_STEPS
    &features::kSteps,
#endif
#if SAS_APP_MEMO
    &features::kMemo,
#endif
#if SAS_APP_ALARM
    &features::kAlarm,
#endif
#if SAS_APP_NOTIFY
    &features::kNotify,
#endif
#if SAS_APP_MEDIA
    &features::kMedia,
#endif
#if SAS_APP_AGENT
    &features::kAgent,
#endif
};
}

const FeatureDescriptor* const* builtin_features() { return kBuiltin; }
size_t builtin_features_count() {
  return sizeof(kBuiltin) / sizeof(kBuiltin[0]);
}

}  // namespace watch
