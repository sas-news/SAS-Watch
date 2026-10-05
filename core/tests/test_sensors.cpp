// Sensors (RaiseDetector/StepDetector) と Steps Feature のホストテスト。
// 判定ロジックはすべて PC で検証する (実機の閾値調整は TODO(hw))。
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "fakes.hpp"
#include "watch/features/steps.hpp"
#include "watch/protocol/dispatch.hpp"
#include "watch/sensors.hpp"
#include "watch/settings.hpp"

using namespace watch;

namespace {

// ImuSample Action を組み立てる (x|y<<16 → arg0, z → arg1)。
Action imu(int16_t x, int16_t y, int16_t z) {
  Action a{};
  a.type = ActionType::ImuSample;
  a.source = ActionSource::System;
  a.arg0 = static_cast<uint16_t>(x) |
           (static_cast<uint32_t>(static_cast<uint16_t>(y)) << 16);
  a.arg1 = static_cast<uint16_t>(z);
  return a;
}

}  // namespace

// ---------- RaiseDetector ----------

TEST(RaiseDetect, FiresOnOrientationChange) {
  sensors::RaiseDetector d;
  // 安静時: 重力 -Z (腕を下ろした状態の推定向き)。
  for (int i = 0; i < 20; ++i) {
    EXPECT_FALSE(d.feed(0, 0, -1000, i * 50));
  }
  // 90° 向きが変わる (0,-1000,0)。hold 200ms 超で検出。
  bool fired = false;
  for (int i = 20; i < 40 && !fired; ++i) {
    fired = d.feed(0, -1000, 0, i * 50);
  }
  EXPECT_TRUE(fired);
  // そのまま続けても再検出しない。
  for (int i = 40; i < 60; ++i) {
    EXPECT_FALSE(d.feed(0, -1000, 0, i * 50));
  }
  // 元の向きに戻る → 再武装。次の変化でまた検出。
  for (int i = 60; i < 90; ++i) {
    d.feed(0, 0, -1000, i * 50);
  }
  bool refired = false;
  for (int i = 90; i < 120 && !refired; ++i) {
    refired = d.feed(0, -1000, 0, i * 50);
  }
  EXPECT_TRUE(refired);
}

TEST(RaiseDetect, TransientDoesNotFire) {
  sensors::RaiseDetector d;
  for (int i = 0; i < 20; ++i) d.feed(0, 0, -1000, i * 50);
  // 100ms だけ向きが変わる (hold 200ms 未満)。
  EXPECT_FALSE(d.feed(0, -1000, 0, 1000));
  EXPECT_FALSE(d.feed(0, -1000, 0, 1050));
  EXPECT_FALSE(d.feed(0, 0, -1000, 1100));
  // 直後は未検出のまま。
  for (int i = 0; i < 10; ++i) {
    EXPECT_FALSE(d.feed(0, 0, -1000, 1150 + i * 50));
  }
}

TEST(RaiseDetect, OutOfRangeSamplesIgnored) {
  sensors::RaiseDetector d;
  for (int i = 0; i < 20; ++i) d.feed(0, 0, -1000, i * 50);
  // 大きな揺れ (2g) の間は判定を進めない。
  for (int i = 20; i < 30; ++i) {
    EXPECT_FALSE(d.feed(0, -2000, 0, i * 50));
  }
  // 揺れが収まっても hold はリセットされているので即検出にはならない。
  int first_at = -1;
  for (int i = 30; i < 60; ++i) {
    if (d.feed(0, -1000, 0, i * 50)) {
      first_at = i;
      break;
    }
  }
  // 検出されるなら 30 以降 200ms 以上先。
  if (first_at >= 0) EXPECT_GE(first_at * 50 - 30 * 50, 200);
}

// ---------- StepDetector ----------

// 歩行っぽいパターン: 大きさが 1400/600 mg を交互に振動 (歩行 ~1 歩/5サンプル)。
// StepDetector は向きを見ないので (0,0,z) で良い。
TEST(StepDetect, WalkingPatternCounts) {
  sensors::StepDetector d;
  int steps = 0;
  int64_t t = 0;
  for (int i = 0; i < 200; ++i) {  // 10秒分 @20Hz
    const int16_t z = static_cast<int16_t>(-1000 + ((i % 10 < 5) ? -400 : 400));
    if (d.feed(0, 0, z, t)) ++steps;
    t += 50;
  }
  // ベースラインへ追従する分を引いても ~20 歩前後を期待。
  EXPECT_GE(steps, 15);
  EXPECT_LE(steps, 25);
}

TEST(StepDetect, QuietDoesNotCount) {
  sensors::StepDetector d;
  int64_t t = 0;
  int steps = 0;
  for (int i = 0; i < 100; ++i) {
    if (d.feed(0, 0, -1000, t)) ++steps;
    t += 50;
  }
  EXPECT_EQ(steps, 0);
}

TEST(StepDetect, RefractoryLimitsRate) {
  sensors::StepDetector d;
  int steps = 0;
  // 100ms ごとに交互 (refractory 350ms で 3-4 個に1個しか数えない)。
  for (int i = 0; i < 40; ++i) {
    const int16_t z = static_cast<int16_t>(-1000 + ((i % 2) ? -400 : 400));
    if (d.feed(0, 0, z, i * 100)) ++steps;
  }
  EXPECT_GE(steps, 1);
  EXPECT_LT(steps, 20);
}

// ---------- Steps Feature ----------

struct StepsFixture {
  EventBus bus;
  test::MemoryKeyValueStore kv;
  test::FakeClock clock;
  Settings settings;
  test::EventRecorder rec;
  FeatureContext ctx{bus, kv, clock, nullptr, nullptr, nullptr, &settings};

  StepsFixture() {
    rec.attach_all(bus);
    features::steps_reset_state();
    features::kSteps.restore(ctx);
  }

  void feed(int16_t x, int16_t y, int16_t z) {
    Action a = imu(x, y, z);
    features::kSteps.handle(a, ctx);
  }
  // 歩行っぽい大きさ振動を n サンプル流す。
  void walk(int n) {
    for (int i = 0; i < n; ++i) {
      const int16_t z =
          static_cast<int16_t>(-1000 + ((i % 10 < 5) ? -400 : 400));
      feed(0, 0, z);
      clock.advance_ms(50);
    }
  }
};

TEST(Steps, ImuSampleCountsAndPublishes) {
  StepsFixture f;
  f.walk(200);
  EXPECT_GT(features::steps_today(), 0u);
  const auto* e = f.rec.last_of(EventType::StepsChanged);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->arg0, features::steps_today());
}

TEST(Steps, RaisePublishesEvent) {
  StepsFixture f;
  // 安静姿勢を数サンプル → 向きを 90° 変える。
  for (int i = 0; i < 20; ++i) {
    f.feed(0, 0, -1000);
    f.clock.advance_ms(50);
  }
  bool raised = false;
  for (int i = 0; i < 40 && !raised; ++i) {
    f.feed(0, -1000, 0);
    raised = f.rec.count_of(EventType::RaiseDetected) > 0;
    f.clock.advance_ms(50);
  }
  EXPECT_TRUE(raised);
}

TEST(Steps, DateRolloverResets) {
  StepsFixture f;
  f.walk(100);
  const uint32_t day1 = features::steps_today();
  EXPECT_GT(day1, 0u);
  // 翌日の世界へ。
  f.clock.set_epoch_s(f.clock.epoch_s() + 86400);
  features::kSteps.tick(f.clock.now_ms(), f.ctx);
  EXPECT_EQ(features::steps_today(), 0u);
  const auto* e = f.rec.last_of(EventType::StepsChanged);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->arg0, 0u);
}

TEST(Steps, RestoreKeepsSameDay) {
  StepsFixture f;
  f.walk(100);
  features::kSteps.save(f.ctx);
  const uint32_t before = features::steps_today();
  EXPECT_GT(before, 0u);

  test::FakeClock clock2;
  clock2.set_epoch_s(f.clock.epoch_s());
  FeatureContext ctx2{f.bus, f.kv, clock2, nullptr, nullptr, nullptr,
                      &f.settings};
  features::kSteps.restore(ctx2);
  EXPECT_EQ(features::steps_today(), before);
}

TEST(Steps, RestoreNextDayIsZero) {
  StepsFixture f;
  f.walk(100);
  features::kSteps.save(f.ctx);

  test::FakeClock clock2;
  clock2.set_epoch_s(f.clock.epoch_s() + 86400);
  FeatureContext ctx2{f.bus, f.kv, clock2, nullptr, nullptr, nullptr,
                      &f.settings};
  features::kSteps.restore(ctx2);
  EXPECT_EQ(features::steps_today(), 0u);
}

TEST(Steps, MidnightDeadlineIsFuture) {
  StepsFixture f;
  const int64_t dl = features::kSteps.next_deadline_ms();
  EXPECT_GT(dl, f.clock.now_ms());
}

// ---------- steps.get (protocol) ----------

TEST(StepsGet, ReturnsStepsAndGoal) {
  EventBus bus;
  test::MemoryKeyValueStore kv;
  test::FakeClock clock;
  Settings settings;
  proto::Services svc{};
  svc.settings = &settings;
  static uint32_t s_fake = 4321;
  s_fake = 4321;
  svc.steps_today = [](void*) { return s_fake; };

  uint8_t req_buf[128];
  cbor::Writer w(req_buf, sizeof(req_buf));
  w.map(2).text("m").text("steps.get").text("p").map(0);
  ASSERT_TRUE(w.ok());
  uint8_t out[128];
  cbor::Writer ow(out, sizeof(out));
  ASSERT_EQ(proto::dispatch_req(req_buf, w.size(), svc, &ow),
            proto::DispatchError::Ok);
  cbor::Value top{out, out + ow.size()}, r, v;
  ASSERT_TRUE(cbor::map_find(top, "r", &r));
  ASSERT_TRUE(cbor::map_find(r, "steps", &v));
  uint64_t u;
  ASSERT_TRUE(cbor::as_uint(v, &u));
  EXPECT_EQ(u, 4321u);
  ASSERT_TRUE(cbor::map_find(r, "goal", &v));
  ASSERT_TRUE(cbor::as_uint(v, &u));
  EXPECT_EQ(u, settings.steps_goal);
}
