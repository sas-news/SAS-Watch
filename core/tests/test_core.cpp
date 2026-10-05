// EventBus / ActionQueue / Navigator / PowerPolicy / InputMapper / Runtime の
// ホストテスト。
#include <gtest/gtest.h>

#include <vector>

#include "watch/feature.hpp"
#include "watch/features/timer.hpp"
#include "watch/input_mapper.hpp"
#include "watch/navigation.hpp"
#include "watch/power.hpp"
#include "watch/runtime.hpp"
#include "fakes.hpp"

using namespace watch;

// ---------- EventBus ----------

TEST(EventBus, PublishDeliversToMatchingSubscribers) {
  EventBus bus;
  test::EventRecorder rec;
  rec.attach(bus, EventType::TimerFinished);
  bus.publish({EventType::TimerStarted, 5});
  bus.publish({EventType::TimerFinished, 0});
  EXPECT_EQ(rec.events.size(), 1u);
  EXPECT_EQ(rec.events[0].type, EventType::TimerFinished);
}

TEST(EventBus, WildcardReceivesEverything) {
  EventBus bus;
  test::EventRecorder rec;
  rec.attach_all(bus);
  bus.publish({EventType::TimerStarted, 1});
  bus.publish({EventType::RouteChanged, 2});
  EXPECT_EQ(rec.events.size(), 2u);
}

TEST(EventBus, UnsubscribeStopsDelivery) {
  EventBus bus;
  test::EventRecorder rec;
  rec.attach(bus, EventType::TimerStarted);
  bus.unsubscribe(&test::EventRecorder::handler, &rec);
  bus.publish({EventType::TimerStarted, 0});
  EXPECT_TRUE(rec.events.empty());
}

TEST(EventBus, FullTableRejects) {
  EventBus bus;
  // 同じ fn でも ctx が違えば別の購読として数える。
  for (size_t i = 0; i < EventBus::kMaxSubscriptions; ++i) {
    EXPECT_TRUE(bus.subscribe(EventType::TimerStarted,
                              &test::EventRecorder::handler,
                              reinterpret_cast<void*>(i + 1)));
  }
  EXPECT_FALSE(bus.subscribe(EventType::TimerStarted,
                             &test::EventRecorder::handler,
                             reinterpret_cast<void*>(0xFFFF)));
}

TEST(EventBus, DuplicateSubscribeIgnored) {
  EventBus bus;
  test::EventRecorder rec;
  EXPECT_TRUE(bus.subscribe(EventType::TimerStarted,
                            &test::EventRecorder::handler, &rec));
  EXPECT_TRUE(bus.subscribe(EventType::TimerStarted,
                            &test::EventRecorder::handler, &rec));
  EXPECT_EQ(bus.count(), 1u);
}

// ---------- ActionQueue ----------

TEST(ActionQueue, Fifo) {
  ActionQueue q;
  Action a1, a2, out;
  a1.type = ActionType::TimerStart;
  a2.type = ActionType::CounterAdd;
  a2.arg0 = 3;
  EXPECT_TRUE(q.push(a1));
  EXPECT_TRUE(q.push(a2));
  EXPECT_TRUE(q.pop(&out));
  EXPECT_EQ(out.type, ActionType::TimerStart);
  EXPECT_TRUE(q.pop(&out));
  EXPECT_EQ(out.type, ActionType::CounterAdd);
  EXPECT_EQ(out.arg0, 3u);
  EXPECT_FALSE(q.pop(&out));
}

TEST(ActionQueue, FullDrops) {
  ActionQueue q;
  Action a;
  for (size_t i = 0; i < ActionQueue::kCapacity; ++i) {
    EXPECT_TRUE(q.push(a));
  }
  EXPECT_FALSE(q.push(a));  // 落とすがクラッシュしない
  EXPECT_EQ(q.count(), ActionQueue::kCapacity);
}

TEST(ActionQueue, LockHooksCalled) {
  ActionQueue q;
  static int lock_count = 0;
  static int unlock_count = 0;
  lock_count = unlock_count = 0;
  q.set_lock_hooks(
      [](void*) { ++lock_count; }, [](void*) { ++unlock_count; }, nullptr);
  Action a, out;
  q.push(a);
  q.pop(&out);
  EXPECT_EQ(lock_count, 2);
  EXPECT_EQ(unlock_count, 2);
}

TEST(ActionQueue, TextSurvivesCopy) {
  ActionQueue q;
  Action a, out;
  a.type = ActionType::MemoCreate;
  a.set_text("hello memo");
  q.push(a);
  q.pop(&out);
  EXPECT_STREQ(out.text, "hello memo");
}

// ---------- Navigator ----------

TEST(Navigator, PushPopReplace) {
  Navigator nav;
  EXPECT_EQ(nav.current(), Route::Home);
  nav.push(Route::Quick);
  nav.push(Route::Timer);
  EXPECT_EQ(nav.current(), Route::Timer);
  EXPECT_EQ(nav.depth(), 3);
  nav.replace(Route::Stopwatch);
  EXPECT_EQ(nav.current(), Route::Stopwatch);
  nav.pop();
  EXPECT_EQ(nav.current(), Route::Quick);
  nav.pop_to_home();
  EXPECT_EQ(nav.current(), Route::Home);
  EXPECT_EQ(nav.depth(), 1);
}

TEST(Navigator, MaxDepthKeepsHomeAndDropsOldest) {
  Navigator nav;
  nav.push(Route::Quick);       // [H Q]
  nav.push(Route::Timer);       // [H Q T]
  nav.push(Route::Stopwatch);   // [H Q T S] (4段)
  nav.push(Route::Counter);     // 溢れ → 古い非Home(Q)を捨てる → [H T S C]
  EXPECT_EQ(nav.depth(), 4);
  EXPECT_EQ(nav.current(), Route::Counter);
  EXPECT_EQ(nav.at(3), Route::Home);  // Home は残る
  EXPECT_EQ(nav.at(2), Route::Timer);
  EXPECT_EQ(nav.at(1), Route::Stopwatch);
}

TEST(Navigator, Modal) {
  Navigator nav;
  nav.push(Route::Agent);
  nav.present_modal(Route::Confirm);
  EXPECT_TRUE(nav.has_modal());
  EXPECT_EQ(nav.modal(), Route::Confirm);
  nav.pop();  // modal が先に閉じる
  EXPECT_FALSE(nav.has_modal());
  EXPECT_EQ(nav.current(), Route::Agent);
  nav.dismiss_modal();
  nav.pop();
  EXPECT_EQ(nav.current(), Route::Home);
}

TEST(Navigator, RouteChangedEvents) {
  EventBus bus;
  test::EventRecorder rec;
  rec.attach_all(bus);
  Navigator nav(&bus);
  nav.push(Route::Timer);
  nav.pop();
  const auto* e = rec.last_of(EventType::RouteChanged);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->arg0, static_cast<uint32_t>(Route::Home));
  EXPECT_EQ(rec.count_of(EventType::RouteChanged), 2u);
}

TEST(Navigator, HandleAction) {
  Navigator nav;
  Action a;
  a.type = ActionType::Navigate;
  a.arg0 = static_cast<uint32_t>(Route::Timer);
  EXPECT_TRUE(nav.handle_action(a));
  EXPECT_EQ(nav.current(), Route::Timer);

  Action back;
  back.type = ActionType::Back;
  EXPECT_TRUE(nav.handle_action(back));   // Timer → Home
  EXPECT_FALSE(nav.handle_action(back));  // Home 末端は false
}

TEST(Navigator, HomeActionResetsStack) {
  Navigator nav;
  nav.push(Route::Quick);
  nav.push(Route::Timer);
  Action home;
  home.type = ActionType::Home;
  EXPECT_TRUE(nav.handle_action(home));
  EXPECT_EQ(nav.depth(), 1);
}

// ---------- PowerPolicy ----------

namespace {
PowerPolicy make_policy(EventBus* bus = nullptr) {
  PowerPolicy p(bus);
  p.configure(8, 12, 1800);  // plan.md L章: 8s dim / +4s off / 30min deep
  return p;
}
}  // namespace

TEST(PowerPolicy, InactivityTransitions) {
  PowerPolicy p = make_policy();
  p.kick_activity(0);
  EXPECT_EQ(p.update(7'999), PowerState::Active);
  EXPECT_EQ(p.update(8'000), PowerState::Dim);
  EXPECT_EQ(p.update(11'999), PowerState::Dim);
  EXPECT_EQ(p.update(12'000), PowerState::ScreenOff);
  EXPECT_EQ(p.update(1'800'000), PowerState::DeepSleepCandidate);
}

TEST(PowerPolicy, KickResetsToActive) {
  PowerPolicy p = make_policy();
  p.kick_activity(0);
  p.update(20'000);
  EXPECT_EQ(p.state(), PowerState::ScreenOff);
  const PowerState before = p.kick_activity(20'100);
  EXPECT_EQ(before, PowerState::ScreenOff);
  EXPECT_EQ(p.state(), PowerState::Active);
  EXPECT_EQ(p.update(20'100 + 9'000), PowerState::Dim);
}

TEST(PowerPolicy, RequestScreenOff) {
  PowerPolicy p = make_policy();
  p.kick_activity(0);
  p.request_screen_off(100);
  EXPECT_EQ(p.state(), PowerState::ScreenOff);
}

TEST(PowerPolicy, LeaseBlocksDeepSleep) {
  PowerPolicy p = make_policy();
  p.kick_activity(0);
  {
    auto lease = p.acquire(Res::Audio, "memo");
    EXPECT_TRUE(lease.valid());
    EXPECT_EQ(p.lease_count(), 1);
    p.update(3'000'000);  // 30分超でも DeepSleep に行かない
    EXPECT_EQ(p.state(), PowerState::ScreenOff);
  }  // RAII release
  EXPECT_EQ(p.lease_count(), 0);
  p.update(3'000'000);
  EXPECT_EQ(p.state(), PowerState::DeepSleepCandidate);
}

TEST(PowerPolicy, LeaseMoveSemantics) {
  PowerPolicy p = make_policy();
  auto a = p.acquire(Res::Wifi, "wifi");
  auto b = std::move(a);
  EXPECT_FALSE(a.valid());
  EXPECT_TRUE(b.valid());
  EXPECT_EQ(p.lease_count(), 1);
  b.release();
  EXPECT_EQ(p.lease_count(), 0);
}

TEST(PowerPolicy, LeaseExhaustion) {
  PowerPolicy p = make_policy();
  std::vector<PowerPolicy::Lease> held;
  for (int i = 0; i < PowerPolicy::kMaxLeases; ++i) {
    held.push_back(p.acquire(Res::CpuMax, "x"));
  }
  EXPECT_EQ(p.lease_count(), PowerPolicy::kMaxLeases);
  auto extra = p.acquire(Res::Wifi, "y");
  EXPECT_FALSE(extra.valid());  // 空き無し
}

TEST(PowerPolicy, BleBlocksDeepSleep) {
  PowerPolicy p = make_policy();
  p.kick_activity(0);
  p.set_ble_connected(true);
  p.update(3'000'000);
  EXPECT_EQ(p.state(), PowerState::ScreenOff);
  p.set_ble_connected(false);
  p.update(3'000'000);
  EXPECT_EQ(p.state(), PowerState::DeepSleepCandidate);
}

TEST(PowerPolicy, LeaseAcquireDuringCandidateDemotes) {
  PowerPolicy p = make_policy();
  p.kick_activity(0);
  p.update(3'000'000);
  EXPECT_EQ(p.state(), PowerState::DeepSleepCandidate);
  auto lease = p.acquire(Res::BleFast, "ble");
  EXPECT_EQ(p.state(), PowerState::ScreenOff);
}

TEST(PowerPolicy, StateChangedEvents) {
  EventBus bus;
  test::EventRecorder rec;
  rec.attach(bus, EventType::PowerStateChanged);
  PowerPolicy p = make_policy(&bus);
  p.kick_activity(0);
  p.update(9'000);
  p.update(13'000);
  EXPECT_EQ(rec.count_of(EventType::PowerStateChanged), 2u);
  const auto* e = rec.last_of(EventType::PowerStateChanged);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->arg0, static_cast<uint32_t>(PowerState::ScreenOff));
}

TEST(PowerPolicy, NextTransition) {
  PowerPolicy p = make_policy();
  p.kick_activity(1000);
  EXPECT_EQ(p.next_transition_ms(), 1000 + 8'000);
  p.update(9'000);
  EXPECT_EQ(p.next_transition_ms(), 1000 + 12'000);
}

// ---------- InputMapper ----------

TEST(InputMapper, DefaultsMatchPlan) {
  InputMapper m;
  EXPECT_STREQ(m.action_name(PhysicalButton::Boot, PressType::Short),
               "primary");
  EXPECT_STREQ(m.action_name(PhysicalButton::Boot, PressType::Long), "nav.dev");
  EXPECT_STREQ(m.action_name(PhysicalButton::Boot, PressType::Double),
               "memo.record");
  EXPECT_STREQ(m.action_name(PhysicalButton::Pwr, PressType::Short), "back");
  EXPECT_STREQ(m.action_name(PhysicalButton::Pwr, PressType::Long),
               "power_menu");
}

TEST(InputMapper, Translate) {
  InputMapper m;
  Action a;
  ASSERT_TRUE(m.translate({PhysicalButton::Pwr, PressType::Long}, &a));
  EXPECT_EQ(a.type, ActionType::Navigate);
  EXPECT_EQ(a.arg0, static_cast<uint32_t>(Route::PowerMenu));
  EXPECT_EQ(a.source, ActionSource::Button);
}

TEST(InputMapper, Remap) {
  InputMapper m;
  EXPECT_TRUE(m.map(PhysicalButton::Boot, PressType::Short, "nav.quick"));
  Action a;
  ASSERT_TRUE(m.translate({PhysicalButton::Boot, PressType::Short}, &a));
  EXPECT_EQ(a.arg0, static_cast<uint32_t>(Route::Quick));
  EXPECT_FALSE(m.map(PhysicalButton::Boot, PressType::Short, "no.such"));
}

TEST(InputMapper, NoneIsNotTranslated) {
  InputMapper m;
  Action a;
  EXPECT_FALSE(m.translate({PhysicalButton::Pwr, PressType::Double}, &a));
}

// ---------- Runtime ----------

namespace {
struct RuntimeFixture {
  EventBus bus;
  Navigator nav{&bus};
  PowerPolicy power{&bus};
  InputMapper input;
  FeatureRegistry registry{builtin_features(), builtin_features_count()};
  Settings settings;
  test::FakeClock clock;
  test::MemoryKeyValueStore kv;
  FeatureContext ctx{bus, kv, clock, &nav, &power};
  Runtime runtime{bus, nav, power, input, registry, settings};

  RuntimeFixture() {
    power.kick_activity(0);
    features::timer_reset_state();
  }
};
}  // namespace

TEST(Runtime, StepDispatchesAction) {
  RuntimeFixture f;
  Action a;
  a.type = ActionType::Navigate;
  a.arg0 = static_cast<uint32_t>(Route::Timer);
  a.source = ActionSource::Touch;
  f.runtime.queue().push(a);
  EXPECT_TRUE(f.runtime.step(10, f.ctx));
  EXPECT_EQ(f.nav.current(), Route::Timer);
  EXPECT_FALSE(f.runtime.step(11, f.ctx));
}

TEST(Runtime, ScreenOffSwallowsWakeInput) {
  RuntimeFixture f;
  f.power.kick_activity(0);
  f.power.update(60'000);  // ScreenOff
  ASSERT_EQ(f.power.state(), PowerState::ScreenOff);
  Action a;
  a.type = ActionType::Navigate;
  a.arg0 = static_cast<uint32_t>(Route::Timer);
  a.source = ActionSource::Button;
  f.runtime.queue().push(a);
  f.clock.set_now_ms(60'100);  // dispatch 内の kick は clock を見る
  f.runtime.step(60'100, f.ctx);
  // 起きただけで Navigate は実行されない
  EXPECT_EQ(f.power.state(), PowerState::Active);
  EXPECT_EQ(f.nav.current(), Route::Home);
}

TEST(Runtime, BackAtHomeTurnsScreenOff) {
  RuntimeFixture f;
  Action a;
  a.type = ActionType::Back;
  a.source = ActionSource::Button;
  f.runtime.queue().push(a);
  f.clock.set_now_ms(100);
  f.runtime.step(100, f.ctx);
  EXPECT_EQ(f.power.state(), PowerState::ScreenOff);
  // 手動消灯は次の tick で Active に戻らない
  f.runtime.step(200, f.ctx);
  EXPECT_EQ(f.power.state(), PowerState::ScreenOff);
}

TEST(Runtime, PrimaryActionGoesToCurrentRouteFeature) {
  RuntimeFixture f;
  Action nav_a;
  nav_a.type = ActionType::Navigate;
  nav_a.arg0 = static_cast<uint32_t>(Route::Timer);
  nav_a.source = ActionSource::Touch;
  f.runtime.queue().push(nav_a);
  f.runtime.step(10, f.ctx);

  Action p;
  p.type = ActionType::PrimaryAction;
  p.source = ActionSource::Button;
  f.runtime.queue().push(p);
  f.runtime.step(20, f.ctx);
  EXPECT_TRUE(features::timer_state().running);
}

TEST(Runtime, NextDeadlineCoversPower) {
  RuntimeFixture f;
  f.power.kick_activity(1000);
  // Timer 停止中は Feature の期限なし → 電源の dim 期限が出る
  EXPECT_EQ(f.runtime.next_deadline_ms(), 1000 + 8'000);
}
