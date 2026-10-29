// test_walk.cpp — ウォーク Feature のホストテスト (GoogleTest)。
// apps.cmake が SDK_APP_TEST_SOURCES として core のテストに取り込む。
// ポーリング条件 (画面が見えている + Active のときだけ 10 秒間隔) と
// 即時更新の間引き (1.2 秒)、walk.state の反映を検査する。
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>

#include "fakes.hpp"
#include "watch/features/walk.hpp"
#include "watch/navigation.hpp"
#include "watch/power.hpp"
#include "watch/settings.hpp"

using watch::Action;
using watch::ActionType;
using watch::EventType;
using watch::FeatureContext;
using watch::Navigator;
using watch::PowerPolicy;
using watch::PowerState;
using watch::Route;
using watch::features::kWalk;
using watch::features::WalkState;
using watch::features::WalkStatus;

namespace {

struct WalkFixture {
  watch::EventBus bus;
  watch::test::MemoryKeyValueStore kv;
  watch::test::FakeClock clock;
  watch::Settings settings;
  Navigator nav{&bus};
  PowerPolicy power{&bus};
  FeatureContext ctx{bus, kv, clock, &nav, &power};
  watch::test::EventRecorder queries;   // WalkQueryRequested
  watch::test::EventRecorder changes;   // WalkStateChanged

  WalkFixture() {
    ctx.settings = &settings;
    queries.attach(bus, EventType::WalkQueryRequested);
    changes.attach(bus, EventType::WalkStateChanged);
    watch::features::walk_reset_state();
    nav.push(Route::App0);  // ウォーク画面 = 現在の画面
  }

  size_t query_count() const {
    return queries.count_of(EventType::WalkQueryRequested);
  }

  void tick(int64_t now_ms) { kWalk.tick(now_ms, ctx); }

  bool refresh() {
    Action a;
    a.type = ActionType::WalkRefresh;
    return kWalk.handle(a, ctx);
  }
};

WalkState sample_state() {
  WalkState s;
  s.status = WalkStatus::Walking;
  std::snprintf(s.route, sizeof(s.route), "○○街道 20km");
  std::snprintf(s.next_cp, sizeof(s.next_cp), "第3関門");
  std::snprintf(s.msg, sizeof(s.msg), "あと少し");
  s.total_m = 20400;
  s.done_m = 12680;
  s.remain_m = 7720;
  s.permille = 622;
  s.elapsed_s = 5400;
  s.eta_epoch = 1761702900;
  s.pace_s_per_km = 720;
  s.started_epoch = 1761697500;
  s.next_cp_m = 320;
  s.off_route_m = 0;
  s.accuracy_m = 8;
  s.updated_epoch = 1761702950;
  return s;
}

}  // namespace

TEST(Walk, PollsEveryTenSecondsWhileVisible) {
  WalkFixture f;
  EXPECT_EQ(kWalk.next_deadline_ms(), 0);  // まだ tick していない

  f.tick(1000);
  EXPECT_EQ(f.query_count(), 1u);
  EXPECT_EQ(kWalk.next_deadline_ms(), 11000);

  // 10 秒未満は送らない。
  f.tick(10999);
  EXPECT_EQ(f.query_count(), 1u);
  EXPECT_EQ(kWalk.next_deadline_ms(), 11000);

  f.tick(11000);
  EXPECT_EQ(f.query_count(), 2u);
  EXPECT_EQ(kWalk.next_deadline_ms(), 21000);
}

TEST(Walk, StopsPollingWhenScreenIsHidden) {
  WalkFixture f;
  f.tick(1000);
  ASSERT_EQ(f.query_count(), 1u);

  f.nav.pop();  // 別の画面へ (Home)
  f.tick(11000);
  EXPECT_EQ(f.query_count(), 1u);
  EXPECT_EQ(kWalk.next_deadline_ms(), 0);

  // 戻ってくればまた送る (次期期限は前回から 10 秒のままなので即時)。
  f.nav.push(Route::App0);
  f.tick(11000);
  EXPECT_EQ(f.query_count(), 2u);
}

TEST(Walk, NoPollWhileDimOrScreenOff) {
  WalkFixture f;
  f.power.configure(8, 12, 1800);
  f.power.kick_activity(0);

  // Active の間は送る。
  f.power.update(1000);
  ASSERT_EQ(f.power.state(), PowerState::Active);
  f.tick(1000);
  ASSERT_EQ(f.query_count(), 1u);

  // Dim (8 秒経過) では送らない。
  f.power.update(9000);
  ASSERT_EQ(f.power.state(), PowerState::Dim);
  f.tick(11000);
  EXPECT_EQ(f.query_count(), 1u);
  EXPECT_EQ(kWalk.next_deadline_ms(), 0);

  // ScreenOff (12 秒経過) でも送らない。
  f.power.update(13000);
  ASSERT_EQ(f.power.state(), PowerState::ScreenOff);
  f.tick(21000);
  EXPECT_EQ(f.query_count(), 1u);
  EXPECT_EQ(kWalk.next_deadline_ms(), 0);
}

TEST(Walk, NoPollWhileModal) {
  WalkFixture f;
  f.nav.present_modal(Route::Confirm);
  f.tick(1000);
  EXPECT_EQ(f.query_count(), 0u);
  EXPECT_EQ(kWalk.next_deadline_ms(), 0);

  f.nav.dismiss_modal();
  f.tick(1000);
  EXPECT_EQ(f.query_count(), 1u);
}

TEST(Walk, RefreshIsThrottled) {
  WalkFixture f;
  f.clock.set_now_ms(1000);
  EXPECT_TRUE(f.refresh());
  EXPECT_EQ(f.query_count(), 1u);

  // 1.2 秒以内は捨てる (画面 build と tick の重なり対策)。
  f.clock.set_now_ms(1500);
  EXPECT_TRUE(f.refresh());  // 処理はした (true) が送らない
  EXPECT_EQ(f.query_count(), 1u);

  f.clock.set_now_ms(2201);
  EXPECT_TRUE(f.refresh());
  EXPECT_EQ(f.query_count(), 2u);
}

TEST(Walk, RefreshMovesNextPollDeadline) {
  WalkFixture f;
  f.clock.set_now_ms(5000);
  f.refresh();
  EXPECT_EQ(kWalk.next_deadline_ms(), 15000);

  // 即時更新のあと 10 秒は tick から二重に送らない。
  f.tick(14000);
  EXPECT_EQ(f.query_count(), 1u);
  f.tick(15000);
  EXPECT_EQ(f.query_count(), 2u);
}

TEST(Walk, RefreshIgnoresOtherActions) {
  WalkFixture f;
  Action a;
  a.type = ActionType::PrimaryAction;
  EXPECT_FALSE(kWalk.handle(a, f.ctx));
  a.type = ActionType::LongPress;
  EXPECT_FALSE(kWalk.handle(a, f.ctx));
  EXPECT_EQ(f.query_count(), 0u);
}

TEST(Walk, SetStatePublishesAndSnapshot) {
  WalkFixture f;
  const WalkState in = sample_state();
  watch::features::walk_set_state(in, f.ctx);

  ASSERT_EQ(f.changes.count_of(EventType::WalkStateChanged), 1u);
  const watch::Event* e = f.changes.last_of(EventType::WalkStateChanged);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->arg0, static_cast<uint32_t>(WalkStatus::Walking));

  const WalkState out = watch::features::walk_state();
  EXPECT_EQ(out.status, WalkStatus::Walking);
  EXPECT_STREQ(out.route, "○○街道 20km");
  EXPECT_STREQ(out.next_cp, "第3関門");
  EXPECT_STREQ(out.msg, "あと少し");
  EXPECT_EQ(out.total_m, 20400u);
  EXPECT_EQ(out.done_m, 12680u);
  EXPECT_EQ(out.remain_m, 7720u);
  EXPECT_EQ(out.permille, 622u);
  EXPECT_EQ(out.elapsed_s, 5400u);
  EXPECT_EQ(out.eta_epoch, 1761702900);
  EXPECT_EQ(out.pace_s_per_km, 720u);
  EXPECT_EQ(out.started_epoch, 1761697500);
  EXPECT_EQ(out.next_cp_m, 320u);
  EXPECT_EQ(out.off_route_m, 0u);
  EXPECT_EQ(out.accuracy_m, 8u);
  EXPECT_EQ(out.updated_epoch, 1761702950);
}

TEST(Walk, ResetClearsStateAndPolling) {
  WalkFixture f;
  watch::features::walk_set_state(sample_state(), f.ctx);
  f.tick(1000);
  ASSERT_EQ(f.query_count(), 1u);
  ASSERT_EQ(kWalk.next_deadline_ms(), 11000);

  watch::features::walk_reset_state();
  const WalkState s = watch::features::walk_state();
  EXPECT_EQ(s.status, WalkStatus::Unknown);
  EXPECT_STREQ(s.route, "");
  EXPECT_EQ(s.total_m, 0u);
  EXPECT_EQ(kWalk.next_deadline_ms(), 0);
}
