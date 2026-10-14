// GENERATED — apps/*/app.cmake と WATCH_EXTRA_APP_DIRS から自動生成。
// 編集しない。再生成は cmake の configure 時。
#include "watch/feature.hpp"
#include "/home/sasnews/Desktop/ai-space/SAS-Watch/core/include/watch/features/clock.hpp"
#include "/home/sasnews/Desktop/ai-space/SAS-Watch/core/include/watch/features/agent.hpp"
#include "/home/sasnews/Desktop/ai-space/SAS-Watch/core/include/watch/features/alarm.hpp"
#include "/home/sasnews/Desktop/ai-space/SAS-Watch/core/include/watch/features/counter.hpp"
#include "/home/sasnews/Desktop/ai-space/SAS-Watch/core/include/watch/features/media.hpp"
#include "/home/sasnews/Desktop/ai-space/SAS-Watch/core/include/watch/features/memo.hpp"
#include "/home/sasnews/Desktop/ai-space/SAS-Watch/core/include/watch/features/notify.hpp"
#include "/home/sasnews/Desktop/ai-space/SAS-Watch/core/include/watch/features/steps.hpp"
#include "/home/sasnews/Desktop/ai-space/SAS-Watch/core/include/watch/features/stopwatch.hpp"
#include "/home/sasnews/Desktop/ai-space/SAS-Watch/core/include/watch/features/timer.hpp"
#include "/tmp/ext_apps/tpl_ext/feature/template_app.hpp"

namespace watch {
namespace {
const FeatureDescriptor* const kAppFeatures[] = {
    &features::kClock,
    &watch::features::kAgent,
    &watch::features::kAlarm,
    &watch::features::kCounter,
    &watch::features::kMedia,
    &watch::features::kMemo,
    &watch::features::kNotify,
    &watch::features::kSteps,
    &watch::features::kStopwatch,
    &watch::features::kTimer,
    &watch::features::kTemplateApp,
};
}
const FeatureDescriptor* const* builtin_features() { return kAppFeatures; }
size_t builtin_features_count() { return sizeof(kAppFeatures) / sizeof(kAppFeatures[0]); }
}  // namespace watch
