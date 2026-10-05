// Feature (Timer/Stopwatch/Counter/Memo) と Settings のホストテスト。
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "watch/features/counter.hpp"
#include "watch/features/memo.hpp"
#include "watch/features/stopwatch.hpp"
#include "watch/features/timer.hpp"
#include "watch/settings.hpp"
#include "fakes.hpp"

using namespace watch;

namespace {

struct FeatFixture {
  EventBus bus;
  test::MemoryKeyValueStore kv;
  test::FakeClock clock;
  test::EventRecorder rec;
  FeatureContext ctx{bus, kv, clock, nullptr, nullptr};

  FeatFixture() {
    rec.attach_all(bus);
    // Feature の static 状態はテスト間で共有なので毎回リセットする。
    features::timer_reset_state();
    features::stopwatch_reset_state();
    features::counter_reset_state();
    features::memo_reset_state();
  }

  void act(const FeatureDescriptor& d, ActionType t, uint32_t arg0 = 0,
           const char* text = nullptr) {
    Action a;
    a.type = t;
    a.arg0 = arg0;
    if (text) a.set_text(text);
    d.handle(a, ctx);
  }
};

}  // namespace

// ---------- Timer ----------

TEST(Timer, StartSetsDeadlineAndPublishes) {
  FeatFixture f;
  f.clock.set_now_ms(1000);
  f.act(features::kTimer, ActionType::TimerStart, 90);
  EXPECT_TRUE(features::timer_state().running);
  EXPECT_EQ(features::timer_state().end_ms, 1000 + 90'000);
  EXPECT_EQ(features::kTimer.next_deadline_ms(), 91'000);
  const auto* e = f.rec.last_of(EventType::TimerStarted);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->arg0, 90u);
}

TEST(Timer, ExpiryPublishesFinished) {
  FeatFixture f;
  f.clock.set_now_ms(0);
  f.act(features::kTimer, ActionType::TimerStart, 10);
  features::kTimer.tick(9'999, f.ctx);
  EXPECT_TRUE(features::timer_state().running);
  features::kTimer.tick(10'000, f.ctx);
  EXPECT_FALSE(features::timer_state().running);
  EXPECT_TRUE(features::timer_state().finished);
  EXPECT_EQ(f.rec.count_of(EventType::TimerFinished), 1u);
}

TEST(Timer, StopCancels) {
  FeatFixture f;
  f.act(features::kTimer, ActionType::TimerStart, 60);
  f.act(features::kTimer, ActionType::TimerStop);
  EXPECT_FALSE(features::timer_state().running);
  EXPECT_EQ(f.rec.count_of(EventType::TimerStopped), 1u);
  EXPECT_EQ(f.rec.count_of(EventType::TimerFinished), 0u);
}

TEST(Timer, DefaultDurationWhenArg0) {
  FeatFixture f;
  f.act(features::kTimer, ActionType::TimerStart, 0);
  EXPECT_EQ(features::timer_state().duration_s, 60u);  // 既定 60s
}

TEST(Timer, PrimaryActionToggles) {
  FeatFixture f;
  f.act(features::kTimer, ActionType::PrimaryAction);
  EXPECT_TRUE(features::timer_state().running);
  f.act(features::kTimer, ActionType::PrimaryAction);
  EXPECT_FALSE(features::timer_state().running);
}

TEST(Timer, RestoreContinuesRunning) {
  FeatFixture f;
  f.clock.set_now_ms(0);
  f.act(features::kTimer, ActionType::TimerStart, 60);
  // 10秒経ってから Deep Sleep 相当で保存。復帰は 30秒後の世界。
  f.clock.advance_ms(10'000);
  features::kTimer.save(f.ctx);
  // 復帰: monotonic は 0 に戻ったが、壁時計は 30秒進んでいる。
  test::FakeClock clock2;
  clock2.set_epoch_s(f.clock.epoch_s() + 30);
  clock2.set_now_ms(0);
  FeatFixture f2;
  f2.kv = f.kv;
  FeatureContext ctx2{f2.bus, f2.kv, clock2, nullptr, nullptr};
  features::kTimer.restore(ctx2);
  EXPECT_TRUE(features::timer_state().running);
  // 残りは 60 - (10 + 30) = 20秒のはず。
  EXPECT_EQ(features::timer_remaining_ms(clock2.now_ms()), 20'000);
}

TEST(Timer, RestoreAfterExpiryFinishesOnTick) {
  FeatFixture f;
  f.clock.set_now_ms(0);
  f.act(features::kTimer, ActionType::TimerStart, 5);
  features::kTimer.save(f.ctx);
  // スリープ中に期限切れ。
  test::FakeClock clock2;
  clock2.set_epoch_s(f.clock.epoch_s() + 100);
  FeatFixture f2;
  f2.kv = f.kv;
  FeatureContext ctx2{f2.bus, f2.kv, clock2, nullptr, nullptr};
  features::kTimer.restore(ctx2);
  features::kTimer.tick(clock2.now_ms(), ctx2);
  EXPECT_TRUE(features::timer_state().finished);
  EXPECT_EQ(f2.rec.count_of(EventType::TimerFinished), 1u);
}

// ---------- Stopwatch ----------

TEST(Stopwatch, TogglePauseElapsed) {
  FeatFixture f;
  f.clock.set_now_ms(0);
  f.act(features::kStopwatch, ActionType::StopwatchToggle);
  EXPECT_TRUE(features::stopwatch_state().running);
  EXPECT_EQ(features::stopwatch_elapsed_ms(5'000), 5'000);
  f.clock.set_now_ms(5'000);
  f.act(features::kStopwatch, ActionType::StopwatchToggle);  // 一時停止
  EXPECT_FALSE(features::stopwatch_state().running);
  EXPECT_EQ(features::stopwatch_elapsed_ms(99'000), 5'000);
}

TEST(Stopwatch, LapsUpTo20) {
  FeatFixture f;
  f.act(features::kStopwatch, ActionType::StopwatchToggle);
  for (int i = 0; i < 25; ++i) {
    f.act(features::kStopwatch, ActionType::StopwatchLap);
  }
  EXPECT_EQ(features::stopwatch_state().lap_count,
            features::kStopwatchMaxLaps);  // 20 で打ち止め
}

TEST(Stopwatch, ResetClears) {
  FeatFixture f;
  f.act(features::kStopwatch, ActionType::StopwatchToggle);
  f.act(features::kStopwatch, ActionType::StopwatchLap);
  f.act(features::kStopwatch, ActionType::StopwatchReset);
  EXPECT_EQ(features::stopwatch_state().lap_count, 0u);
  EXPECT_EQ(features::stopwatch_elapsed_ms(0), 0);
}

TEST(Stopwatch, SaveRestore) {
  FeatFixture f;
  f.clock.set_now_ms(1'000);
  f.act(features::kStopwatch, ActionType::StopwatchToggle);
  f.clock.set_now_ms(4'000);
  f.act(features::kStopwatch, ActionType::StopwatchLap);
  features::kStopwatch.save(f.ctx);
  FeatFixture f2;
  f2.kv = f.kv;
  features::kStopwatch.restore(f2.ctx);
  EXPECT_TRUE(features::stopwatch_state().running);
  EXPECT_EQ(features::stopwatch_state().lap_count, 1u);
  EXPECT_EQ(features::stopwatch_elapsed_ms(f2.clock.now_ms()), 3'000);
}

// ---------- Counter ----------

TEST(Counter, AddSubReset) {
  FeatFixture f;
  f.act(features::kCounter, ActionType::CounterAdd, 1);
  f.act(features::kCounter, ActionType::CounterAdd, 1);
  f.act(features::kCounter, ActionType::CounterAdd,
        static_cast<uint32_t>(-1));
  EXPECT_EQ(features::counter_value(), 1);
  f.act(features::kCounter, ActionType::CounterReset);
  EXPECT_EQ(features::counter_value(), 0);
}

TEST(Counter, SaveRestore) {
  FeatFixture f;
  f.act(features::kCounter, ActionType::CounterAdd, 7);
  features::kCounter.save(f.ctx);
  FeatFixture f2;
  f2.kv = f.kv;
  features::kCounter.restore(f2.ctx);
  EXPECT_EQ(features::counter_value(), 7);
}

// ---------- Memo ----------

TEST(Memo, CreateAndRead) {
  FeatFixture f;
  EXPECT_EQ(features::memo_create("alpha", 5, f.ctx), 1);
  EXPECT_EQ(features::memo_create("beta", 4, f.ctx), 2);
  EXPECT_EQ(features::memo_count(), 2u);
  features::MemoEntry e;
  ASSERT_TRUE(features::memo_at(0, &e));
  EXPECT_STREQ(e.text, "alpha");
  ASSERT_TRUE(features::memo_find(2, &e));
  EXPECT_STREQ(e.text, "beta");
  const auto* ev = f.rec.last_of(EventType::MemoSaved);
  ASSERT_NE(ev, nullptr);
  EXPECT_EQ(ev->arg0, 2u);
}

TEST(Memo, OverflowEvictsOldest) {
  FeatFixture f;
  for (size_t i = 0; i < features::kMemoMaxEntries; ++i) {
    std::string s = "m" + std::to_string(i);
    EXPECT_GT(features::memo_create(s.c_str(), s.size(), f.ctx), 0);
  }
  EXPECT_EQ(features::memo_count(), features::kMemoMaxEntries);
  // 65件目 → 最古 (id=1) が消える。
  features::memo_create("new", 3, f.ctx);
  EXPECT_EQ(features::memo_count(), features::kMemoMaxEntries);
  EXPECT_FALSE(features::memo_find(1, nullptr));
  features::MemoEntry e;
  ASSERT_TRUE(features::memo_at(0, &e));
  EXPECT_EQ(e.id, 2u);  // 2番目が最古に
}

TEST(Memo, RejectsTooLong) {
  FeatFixture f;
  char big[features::kMemoMaxText + 2];
  std::memset(big, 'x', sizeof(big));
  EXPECT_LT(features::memo_create(big, sizeof(big), f.ctx), 0);
  char ok[features::kMemoMaxText];
  std::memset(ok, 'y', sizeof(ok));
  EXPECT_GT(features::memo_create(ok, sizeof(ok), f.ctx), 0);
  features::MemoEntry e;
  ASSERT_TRUE(features::memo_find(features::memo_count(), &e));
  EXPECT_EQ(e.len, features::kMemoMaxText);
}

TEST(Memo, RestorePreservesOrder) {
  FeatFixture f;
  features::memo_create("one", 3, f.ctx);
  features::memo_create("two", 3, f.ctx);
  FeatFixture f2;
  f2.kv = f.kv;
  features::kMemo.restore(f2.ctx);
  EXPECT_EQ(features::memo_count(), 2u);
  features::MemoEntry e;
  ASSERT_TRUE(features::memo_at(1, &e));
  EXPECT_STREQ(e.text, "two");
  EXPECT_EQ(e.id, 2u);
}

TEST(Memo, CreateViaAction) {
  FeatFixture f;
  f.act(features::kMemo, ActionType::MemoCreate, 0, "via action");
  EXPECT_EQ(features::memo_count(), 1u);
}

// ---------- Settings ----------

TEST(Settings, Defaults) {
  Settings s;
  EXPECT_EQ(s.brightness, 50u);
  EXPECT_EQ(s.dim_after_s, 8u);
  EXPECT_EQ(s.screen_off_after_s, 12u);
  EXPECT_EQ(s.deep_sleep_after_s, 1800u);
  EXPECT_STREQ(s.theme, "standard");
  EXPECT_STREQ(s.button_pwr_short, "back");
}

TEST(Settings, PersistRoundTrip) {
  test::MemoryKeyValueStore kv;
  Settings s;
  EXPECT_TRUE(settings_set_u32(s, &kv, "brightness", 80));
  EXPECT_TRUE(settings_set_str(s, &kv, "theme", "dark"));
  EXPECT_TRUE(settings_set_str(s, &kv, "button.pwr.long", "nav.dev"));

  Settings s2;
  settings_load(s2, kv);
  EXPECT_EQ(s2.brightness, 80u);
  EXPECT_STREQ(s2.theme, "dark");
  EXPECT_STREQ(s2.button_pwr_long, "nav.dev");
  EXPECT_EQ(s2.dim_after_s, 8u);  // 未設定は既定値のまま
}

TEST(Settings, UnknownKeyAndBadValueRejected) {
  test::MemoryKeyValueStore kv;
  Settings s;
  EXPECT_FALSE(settings_set_u32(s, &kv, "no.such", 1));
  char long_str[64];
  std::memset(long_str, 'a', sizeof(long_str));
  long_str[sizeof(long_str) - 1] = '\0';
  EXPECT_FALSE(settings_set_str(s, &kv, "theme", long_str));  // 32上限
}

TEST(Settings, SaveAllAndLoad) {
  test::MemoryKeyValueStore kv;
  Settings s;
  s.brightness = 10;
  s.tz_offset_min = -300;
  settings_save_all(s, kv);
  Settings s2;
  settings_load(s2, kv);
  EXPECT_EQ(s2.brightness, 10u);
  EXPECT_EQ(s2.tz_offset_min, -300);
}
