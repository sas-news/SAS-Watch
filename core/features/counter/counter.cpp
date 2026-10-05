// Counter Feature。+/-/リセットだけ。
#include "watch/features/counter.hpp"
#include <cstring>

namespace watch {
namespace features {

namespace {

int32_t g_value = 0;
constexpr const char* kKey = "feat.counter";

bool handle(const Action& a, FeatureContext& ctx) {
  switch (a.type) {
    case ActionType::CounterAdd: {
      // arg0 を符号付きとして解釈する (+1/-1 など)。
      g_value += static_cast<int32_t>(a.arg0);
      ctx.bus.publish(
          {EventType::CounterChanged, static_cast<uint32_t>(g_value)});
      return true;
    }
    case ActionType::CounterReset:
      g_value = 0;
      ctx.bus.publish({EventType::CounterChanged, 0});
      return true;
    case ActionType::PrimaryAction:
      // Counter 画面の主アクション = +1。
      ++g_value;
      ctx.bus.publish(
          {EventType::CounterChanged, static_cast<uint32_t>(g_value)});
      return true;
    default:
      return false;
  }
}

void save(FeatureContext& ctx) {
  int32_t v = g_value;
  ctx.storage.set(kKey, &v, sizeof(v));
}

void restore(FeatureContext& ctx) {
  // スタック変数のアドレスを KV 読み出しに渡すと GCC13 の
  // -Wdangling-pointer を踏むので、初期化時のみの復元先は静的にする。
  static int32_t v;
  v = 0;
  size_t n = 0;
  if (ctx.storage.get(kKey, &v, sizeof(v), &n) && n == sizeof(v)) {
    g_value = v;
  }
}

}  // namespace

int32_t counter_value() { return g_value; }

void counter_reset_state() { g_value = 0; }

const FeatureDescriptor kCounter = {
    /*id*/ "counter",
    /*capabilities*/ HasScreen | HasQuickTile,
    /*route*/ Route::Counter,
    /*init*/ nullptr,
    /*handle*/ handle,
    /*tick*/ nullptr,
    /*next_deadline_ms*/ nullptr,
    /*save*/ save,
    /*restore*/ restore,
};

}  // namespace features
}  // namespace watch
