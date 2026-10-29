// walk.cpp — 歩く大会モード (Walk) の Feature。
//   状態はスマホが正。時計は「ウォーク画面が Active で見えている間だけ」
//   10 秒間隔で EVT walk.query を出す (Dim / ScreenOff / 別画面では出さない)。
//   画面を組み立てた直後と「いま更新」は WalkRefresh で即時要求する
//   (最短 1.2 秒間隔で間引く)。docs/walk-event.md §3〜§4。
//   永続化はしない: 状態は次の問い合わせで取り直す。
#include "watch/features/walk.hpp"

#include <cstring>

#include "watch/event.hpp"
#include "watch/power.hpp"

namespace watch {
namespace features {

namespace {

constexpr int64_t kPollIntervalMs = 10000;
constexpr int64_t kRefreshThrottleMs = 1200;

WalkState s_state;          // 直近の walk.state (スマホが正)
bool s_polling = false;     // 今 tick で画面が見えているか
int64_t s_next_poll_ms = 0;  // 次のポーリング期限 (monotonic)
int64_t s_last_query_ms = -kRefreshThrottleMs;  // 最後に query を出した時刻

// walk.query (EVT) を出し、次の期限を進める。
// 呼べるのは画面が見えているときだけ (tick / 画面 build の WalkRefresh) なので、
// 次期期限が app タスクに伝わるよう polling も立てておく。
void publish_query(FeatureContext& c, int64_t now_ms) {
  c.bus.publish({EventType::WalkQueryRequested, 0});
  s_last_query_ms = now_ms;
  s_next_poll_ms = now_ms + kPollIntervalMs;
  s_polling = true;
}

// 10 秒ポーリング。条件は docs/walk-event.md §4.5 のとおり。
void tick(int64_t now_ms, FeatureContext& c) {
  const bool visible = c.nav && c.nav->current() == kWalk.route &&
                       !c.nav->has_modal() && c.power &&
                       c.power->state() == PowerState::Active;
  s_polling = visible;
  if (!visible) return;
  if (now_ms >= s_next_poll_ms) publish_query(c, now_ms);
}

int64_t next_deadline() { return s_polling ? s_next_poll_ms : 0; }

bool handle(const Action& a, FeatureContext& c) {
  if (a.type != ActionType::WalkRefresh) return false;
  walk_request_refresh(c);
  return true;
}

}  // namespace

void walk_request_refresh(FeatureContext& c) {
  const int64_t now_ms = c.clock.now_ms();
  // 1.2 秒以内の連打 (画面 build と tick の重なり) は捨てる。
  if (now_ms - s_last_query_ms < kRefreshThrottleMs) return;
  publish_query(c, now_ms);
}

WalkState walk_state() { return s_state; }

void walk_set_state(const WalkState& s, FeatureContext& c) {
  s_state = s;
  c.bus.publish(
      {EventType::WalkStateChanged, static_cast<uint32_t>(s.status)});
}

void walk_reset_state() {
  s_state = WalkState{};
  s_polling = false;
  s_next_poll_ms = 0;
  s_last_query_ms = -kRefreshThrottleMs;
}

const FeatureDescriptor kWalk = {
    /*id*/ "walk",
    /*capabilities*/ HasScreen,
    /*route*/ Route::App0,  // app.cmake で APP_ROUTE を省略 → 自動採番
    /*init*/ nullptr,
    /*handle*/ handle,
    /*tick*/ tick,
    /*next_deadline_ms*/ next_deadline,
    /*save*/ nullptr,     // 状態はスマホが正 (再起動後は取り直す)
    /*restore*/ nullptr,
};

}  // namespace features
}  // namespace watch
