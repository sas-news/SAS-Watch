// Timer Feature。終了時刻ベースなのでスリープ/再起動でずれない
// (plan.md E章)。Deep Sleep からの復帰は epoch 秒で再計算する。
#include "watch/features/timer.hpp"
#include <cstring>

namespace watch {
namespace features {

namespace {

TimerState g_st;
constexpr uint32_t kDefaultDurationS = 60;
constexpr uint32_t kMaxDurationS = 24 * 60 * 60;  // 24h まで

// KV に保存する形。version を頭に入れて将来変えられるようにする。
// v2: reserved を paused に置き換え、末尾に paused_ms を追加。
// v1 データは paused=0 / paused_ms=0 として読む (旧バイナリを残しても壊れない)。
struct Persist {
  uint8_t version;
  uint8_t running;
  uint8_t finished;
  uint8_t paused;
  int32_t duration_s;
  int64_t end_epoch_s;  // 壁時計での終了時刻
  int32_t end_frac_ms;  // end のミリ秒端数
  int64_t paused_ms;    // 一時停止時点の残り
};
constexpr uint8_t kPersistVersion = 2;
constexpr const char* kKey = "feat.timer";

void start(uint32_t seconds, FeatureContext& ctx) {
  g_st.duration_s = seconds;
  g_st.end_ms = ctx.clock.now_ms() + static_cast<int64_t>(seconds) * 1000;
  g_st.running = true;
  g_st.finished = false;
  g_st.paused = false;
  g_st.paused_ms = 0;
  ctx.bus.publish({EventType::TimerStarted, g_st.duration_s});
}

void resume(FeatureContext& ctx) {
  g_st.end_ms = ctx.clock.now_ms() + g_st.paused_ms;
  g_st.running = true;
  g_st.paused = false;
  ctx.bus.publish({EventType::TimerStarted,
                   static_cast<uint32_t>((g_st.paused_ms + 500) / 1000)});
  g_st.paused_ms = 0;
}

void pause(FeatureContext& ctx) {
  if (!g_st.running) return;
  const int64_t remain = g_st.end_ms - ctx.clock.now_ms();
  g_st.paused_ms = remain > 0 ? remain : 0;
  g_st.running = false;
  g_st.paused = true;
  ctx.bus.publish(
      {EventType::TimerPaused,
       static_cast<uint32_t>((g_st.paused_ms + 500) / 1000)});
}

void stop(FeatureContext& ctx) {
  g_st.running = false;
  g_st.finished = false;
  g_st.paused = false;
  g_st.paused_ms = 0;
  ctx.bus.publish({EventType::TimerStopped, 0});
}

bool handle(const Action& a, FeatureContext& ctx) {
  switch (a.type) {
    case ActionType::TimerStart: {
      if (g_st.paused) {  // arg0=0 で再開 (一時停止の解除)
        resume(ctx);
        return true;
      }
      uint32_t s = a.arg0;
      if (s == 0) s = g_st.duration_s ? g_st.duration_s : kDefaultDurationS;
      if (s > kMaxDurationS) s = kMaxDurationS;
      start(s, ctx);
      return true;
    }
    case ActionType::TimerStop:
      stop(ctx);
      return true;
    case ActionType::TimerPause:
      pause(ctx);
      return true;
    case ActionType::TimerAddMinute:
      // 「もう1分」: 実行中は終了時刻を延ばし、停止/終了/一時停止は 60 秒で開始。
      if (g_st.running) {
        g_st.end_ms += 60'000;
        g_st.duration_s += 60;
        if (g_st.duration_s > kMaxDurationS) {
          g_st.duration_s = kMaxDurationS;
        }
        ctx.bus.publish({EventType::TimerStarted, g_st.duration_s});
      } else if (g_st.paused) {
        g_st.paused_ms += 60'000;
        ctx.bus.publish({EventType::TimerPaused,
                         static_cast<uint32_t>(g_st.paused_ms / 1000)});
      } else {
        start(60, ctx);
      }
      return true;
    case ActionType::TimerReset:
      g_st.running = false;
      g_st.finished = false;
      g_st.paused = false;
      g_st.paused_ms = 0;
      ctx.bus.publish({EventType::TimerStopped, 0});
      return true;
    case ActionType::PrimaryAction:
      // Timer 画面の主アクション = 開始/一時停止トグル。
      if (g_st.running) {
        pause(ctx);
      } else if (g_st.paused) {
        resume(ctx);
      } else {
        start(g_st.duration_s ? g_st.duration_s : kDefaultDurationS, ctx);
      }
      return true;
    default:
      return false;
  }
}

void tick(int64_t now_ms, FeatureContext& ctx) {
  if (g_st.running && now_ms >= g_st.end_ms) {
    g_st.running = false;
    g_st.finished = true;
    ctx.bus.publish({EventType::TimerFinished, 0});
  }
}

int64_t next_deadline() { return g_st.running ? g_st.end_ms : 0; }

void save(FeatureContext& ctx) {
  // 残り時間を壁時計の終了時刻に変換して保存する。
  // monotonic の now は deep sleep で止まる/リセットされるため。
  Persist p{};
  p.version = kPersistVersion;
  p.running = g_st.running ? 1 : 0;
  p.finished = g_st.finished ? 1 : 0;
  p.paused = g_st.paused ? 1 : 0;
  p.duration_s = static_cast<int32_t>(g_st.duration_s);
  if (g_st.running) {
    const int64_t remain_ms = g_st.end_ms - ctx.clock.now_ms();
    p.end_epoch_s = ctx.clock.epoch_s() + remain_ms / 1000;
    p.end_frac_ms = static_cast<int32_t>(remain_ms % 1000);
  } else {
    p.end_epoch_s = 0;
    p.end_frac_ms = 0;
  }
  p.paused_ms = g_st.paused ? g_st.paused_ms : 0;
  ctx.storage.set(kKey, &p, sizeof(p));
}

void restore(FeatureContext& ctx) {
  // スタック変数のアドレスを KV 読み出しに渡すと GCC13 の
  // -Wdangling-pointer を踏むので、初期化時のみの復元先は静的にする。
  static Persist p;
  p = Persist{};
  size_t n = 0;
  // v1 (20B) は paused フィールドが無いだけなので先頭 20B あれば読める。
  if (!ctx.storage.get(kKey, &p, sizeof(p), &n) || n < 20 ||
      p.version < 1 || p.version > kPersistVersion) {
    return;
  }
  g_st.duration_s = p.duration_s > 0 ? static_cast<uint32_t>(p.duration_s)
                                   : kDefaultDurationS;
  g_st.finished = p.finished != 0;
  // v1 データは paused 位置が reserved=0、末尾の paused_ms も読めないので 0
  // (n < sizeof(p) なら pause 関連は 0 のままにしてある)。
  g_st.paused = (p.version >= 2 && n >= sizeof(p) && p.paused != 0);
  g_st.paused_ms = g_st.paused ? p.paused_ms : 0;
  if (g_st.paused) {
    g_st.running = false;
    g_st.end_ms = 0;
    return;
  }
  if (p.running) {
    // 保存時の「壁時計での終了時刻」→ 現在の epoch で残りを再計算。
    const int64_t remain_ms =
        (p.end_epoch_s - ctx.clock.epoch_s()) * 1000 + p.end_frac_ms;
    if (remain_ms > 0) {
      g_st.end_ms = ctx.clock.now_ms() + remain_ms;
      g_st.running = true;
      g_st.finished = false;
    } else {
      // スリープ中に期限を越えている → 次の tick で TimerFinished。
      g_st.end_ms = ctx.clock.now_ms();
      g_st.running = true;
    }
  } else {
    g_st.running = false;
    g_st.end_ms = 0;
  }
}

}  // namespace

const TimerState& timer_state() { return g_st; }

int64_t timer_remaining_ms(int64_t now_ms) {
  if (g_st.paused) return g_st.paused_ms;
  if (!g_st.running) return 0;
  const int64_t r = g_st.end_ms - now_ms;
  return r > 0 ? r : 0;
}

void timer_reset_state() { g_st = TimerState{}; }

const FeatureDescriptor kTimer = {
    /*id*/ "timer",
    /*capabilities*/ HasScreen | HasHomeWidget | HasQuickTile,
    /*route*/ Route::Timer,
    /*init*/ nullptr,
    /*handle*/ handle,
    /*tick*/ tick,
    /*next_deadline_ms*/ next_deadline,
    /*save*/ save,
    /*restore*/ restore,
};

}  // namespace features
}  // namespace watch
