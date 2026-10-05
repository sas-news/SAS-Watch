// Stopwatch Feature。ラップは最大20件 (溢れた分は捨てる)。
#include "watch/features/stopwatch.hpp"
#include <cstring>

namespace watch {
namespace features {

namespace {

StopwatchState g_st;
constexpr const char* kKey = "feat.stopwatch";

struct Persist {
  uint8_t version;
  uint8_t running;
  uint8_t lap_count;
  uint8_t reserved;
  int64_t acc_ms;
  int64_t started_epoch_offset_ms;  // started_ms - (epoch*1000) は使わず、
                                    // 「保存時点の経過 ms」だけ持つ
};
constexpr uint8_t kPersistVersion = 1;

void toggle(FeatureContext& ctx) {
  if (g_st.running) {
    g_st.acc_ms += ctx.clock.now_ms() - g_st.started_ms;
    g_st.running = false;
    ctx.bus.publish({EventType::StopwatchChanged, 0});
  } else {
    g_st.started_ms = ctx.clock.now_ms();
    g_st.running = true;
    ctx.bus.publish({EventType::StopwatchChanged, 1});
  }
}

void lap(FeatureContext& ctx) {
  if (g_st.lap_count >= kStopwatchMaxLaps) return;  // 溢れた分は捨てる
  g_st.laps[g_st.lap_count++] = stopwatch_elapsed_ms(ctx.clock.now_ms());
  ctx.bus.publish({EventType::StopwatchChanged, 2});
}

void reset(FeatureContext& ctx) {
  g_st = StopwatchState{};
  ctx.bus.publish({EventType::StopwatchChanged, 0});
}

bool handle(const Action& a, FeatureContext& ctx) {
  switch (a.type) {
    case ActionType::StopwatchToggle:
      toggle(ctx);
      return true;
    case ActionType::StopwatchLap:
      lap(ctx);
      return true;
    case ActionType::StopwatchReset:
      reset(ctx);
      return true;
    case ActionType::PrimaryAction:
      // Stopwatch 画面の主アクション = 開始/一時停止トグル。
      toggle(ctx);
      return true;
    default:
      return false;
  }
}

void save(FeatureContext& ctx) {
  // monotonic はリセットされるので「保存時点の経過 ms」を持つ。
  Persist p{};
  p.version = kPersistVersion;
  p.running = g_st.running ? 1 : 0;
  p.lap_count = static_cast<uint8_t>(g_st.lap_count);
  p.acc_ms = stopwatch_elapsed_ms(ctx.clock.now_ms());
  p.started_epoch_offset_ms = 0;
  ctx.storage.set(kKey, &p, sizeof(p));
  // ラップは別キーにまとめて (ラップ無しなら消す)。
  if (g_st.lap_count > 0) {
    ctx.storage.set("feat.stopwatch.laps", g_st.laps,
                    g_st.lap_count * sizeof(g_st.laps[0]));
  } else {
    ctx.storage.erase("feat.stopwatch.laps");
  }
}

void restore(FeatureContext& ctx) {
  // GCC13 の -Wdangling-pointer 対策: p のアドレスは KV に渡さない。
  uint8_t raw[sizeof(Persist)] = {};
  size_t n = 0;
  if (!ctx.storage.get(kKey, raw, sizeof(raw), &n) || n < sizeof(Persist)) {
    return;
  }
  Persist p{};
  std::memcpy(&p, raw, sizeof(p));
  if (p.version != kPersistVersion) {
    return;
  }
  g_st.acc_ms = p.acc_ms;
  g_st.lap_count = p.lap_count <= kStopwatchMaxLaps ? p.lap_count
                                                  : kStopwatchMaxLaps;
  if (g_st.lap_count > 0) {
    size_t ln = 0;
    ctx.storage.get("feat.stopwatch.laps", g_st.laps, sizeof(g_st.laps), &ln);
  }
  if (p.running) {
    // 走ったままスリープしたものは「積み上げ直後から再開」とみなす。
    // deep sleep 中の時間は monotonic が止まるため計測から落ちる
    // (stopwatch の睡眠中計測は RTC メモリ運用で詰める予定)。
    // TODO(hw): 実機で確認 - deep sleep 中も計測継続するか RTC 連携で決める
    g_st.started_ms = ctx.clock.now_ms();
    g_st.running = true;
  } else {
    g_st.running = false;
    g_st.started_ms = 0;
  }
}

}  // namespace

const StopwatchState& stopwatch_state() { return g_st; }

int64_t stopwatch_elapsed_ms(int64_t now_ms) {
  if (!g_st.running) return g_st.acc_ms;
  return g_st.acc_ms + (now_ms - g_st.started_ms);
}

void stopwatch_reset_state() { g_st = StopwatchState{}; }

const FeatureDescriptor kStopwatch = {
    /*id*/ "stopwatch",
    /*capabilities*/ HasScreen | HasQuickTile,
    /*route*/ Route::Stopwatch,
    /*init*/ nullptr,
    /*handle*/ handle,
    /*tick*/ nullptr,
    /*next_deadline_ms*/ nullptr,
    /*save*/ save,
    /*restore*/ restore,
};

}  // namespace features
}  // namespace watch
