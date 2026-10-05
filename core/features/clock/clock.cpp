// 時計 Feature。表示は UI の仕事。ここは1秒ごとに ClockTick を出すだけ。
#include "watch/features/clock.hpp"

namespace watch {
namespace features {

namespace {
int64_t g_last_epoch_s = -1;
}

int64_t clock_last_epoch_s() { return g_last_epoch_s; }

namespace {

void init(FeatureContext& ctx) { g_last_epoch_s = ctx.clock.epoch_s(); }

void tick(int64_t /*now_ms*/, FeatureContext& ctx) {
  const int64_t e = ctx.clock.epoch_s();
  if (e != g_last_epoch_s) {
    g_last_epoch_s = e;
    ctx.bus.publish(
        {EventType::ClockTick, static_cast<uint32_t>(e & 0xFFFFFFFF)});
  }
}

}  // namespace

const FeatureDescriptor kClock = {
    /*id*/ "clock",
    /*capabilities*/ HasScreen | HasHomeWidget,
    /*route*/ Route::Home,
    /*init*/ init,
    /*handle*/ nullptr,
    /*tick*/ tick,
    /*next_deadline_ms*/ nullptr,
    /*save*/ nullptr,
    /*restore*/ nullptr,
};

}  // namespace features
}  // namespace watch
