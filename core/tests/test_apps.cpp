// Feature (Alarm/Notify/Media) と alarm.* メソッドのホストテスト。
#include <gtest/gtest.h>

#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "watch/features/alarm.hpp"
#include "watch/features/media.hpp"
#include "watch/features/notify.hpp"
#include "watch/protocol/dispatch.hpp"
#include "watch/settings.hpp"
#include "fakes.hpp"

using namespace watch;
using watch::features::AlarmEntry;
using watch::features::MediaState;
using watch::features::NotifyEntry;

namespace {

struct AppsFixture {
  EventBus bus;
  test::MemoryKeyValueStore kv;
  test::FakeClock clock;
  Settings settings;
  test::EventRecorder rec;
  FeatureContext ctx{bus, kv, clock, nullptr, nullptr};

  AppsFixture() {
    rec.attach_all(bus);
    ctx.settings = &settings;
    features::alarm_reset_state();
    features::notify_reset_state();
    features::media_reset_state();
  }

  void act(const FeatureDescriptor& d, ActionType t, uint32_t arg0 = 0) {
    Action a;
    a.type = t;
    a.arg0 = arg0;
    d.handle(a, ctx);
  }
};

}  // namespace

// ---------- Alarm ----------

TEST(Alarm, SetListDelete) {
  AppsFixture f;
  const int32_t id1 = features::alarm_set(0, 7, 30, 0, true, f.ctx);
  EXPECT_GT(id1, 0);
  const int32_t id2 = features::alarm_set(0, 8, 0, 0x7F, true, f.ctx);
  EXPECT_GT(id2, id1);
  EXPECT_EQ(features::alarm_count(), 2u);

  AlarmEntry e;
  ASSERT_TRUE(features::alarm_find(static_cast<uint32_t>(id1), &e));
  EXPECT_EQ(e.hour, 7);
  EXPECT_EQ(e.min, 30);
  EXPECT_EQ(e.dow, 0);
  EXPECT_EQ(e.enabled, 1);

  // 更新 (同じ id で上書き)
  EXPECT_EQ(features::alarm_set(static_cast<uint32_t>(id1), 9, 15, 1, false,
                                f.ctx),
            id1);
  ASSERT_TRUE(features::alarm_find(static_cast<uint32_t>(id1), &e));
  EXPECT_EQ(e.hour, 9);
  EXPECT_EQ(e.dow, 1);
  EXPECT_EQ(e.enabled, 0);

  EXPECT_EQ(features::alarm_delete(static_cast<uint32_t>(id1), f.ctx), 1);
  EXPECT_EQ(features::alarm_count(), 1u);
  EXPECT_EQ(features::alarm_delete(static_cast<uint32_t>(id1), f.ctx), 0);
}

TEST(Alarm, MaxFiveAndErrors) {
  AppsFixture f;
  for (int i = 0; i < 5; ++i) {
    EXPECT_GT(features::alarm_set(0, static_cast<uint8_t>(i), 0, 0, true, f.ctx),
              0);
  }
  EXPECT_EQ(features::alarm_set(0, 10, 0, 0, true, f.ctx), -2);  // 満杯
  EXPECT_EQ(features::alarm_set(0, 24, 0, 0, true, f.ctx), -1);  // hour 範囲外
  EXPECT_EQ(features::alarm_set(0, 0, 60, 0, true, f.ctx), -1);  // min 範囲外
  EXPECT_EQ(features::alarm_set(0, 0, 0, 0x80, true, f.ctx), -1);  // dow 範囲外
  EXPECT_EQ(features::alarm_set(999, 1, 0, 0, true, f.ctx), -3);  // 無い id
}

TEST(Alarm, NextFireDaily) {
  AppsFixture f;
  f.clock.set_epoch_s(1'759'657'260);  // 2025-10-05 09:01:00 UTC
  f.settings.tz_offset_min = 540;      // JST (+9h) → local 18:01
  features::alarm_set(0, 19, 0, 0, true, f.ctx);  // ローカル 19:00
  const int64_t fire = features::alarm_next_fire_epoch();
  // ローカル 18:41 → 19:00 はあと 19 分 (1140 秒)
  EXPECT_EQ(fire, 1'759'657'260 + 1140);
  // monotonic deadline は残り時間分だけ先
  EXPECT_GT(features::kAlarm.next_deadline_ms(), 0);
}

TEST(Alarm, FiresAtEpoch) {
  AppsFixture f;
  // epoch 1,000 → JST に合わせない (tz=0) ので local = UTC。
  // now = day 0(木) 00:16:40 → 00:17 ちょうどを設定
  f.clock.set_epoch_s(1020);  // 00:17:00
  f.settings.tz_offset_min = 0;
  features::alarm_set(0, 0, 17, 0, true, f.ctx);
  // 設定した「今の分」は鳴る対象
  EXPECT_EQ(features::alarm_next_fire_epoch(), 1020);
  features::kAlarm.tick(60'000, f.ctx);
  EXPECT_TRUE(features::alarm_ringing());
  EXPECT_EQ(f.rec.count_of(EventType::AlarmRinging), 1u);
}

TEST(Alarm, DowRepeat) {
  AppsFixture f;
  // day0 = 1000 (00:00:00 UTC), 1970-01-01 = 木曜 (wday=4)
  f.clock.set_epoch_s(1000);
  f.settings.tz_offset_min = 0;
  // 金曜 (wday=5) 01:00 だけ → bit5 = 0x20
  features::alarm_set(0, 1, 0, 0x20, true, f.ctx);
  // 次の金曜は day1 → epoch = 86400 + 3600
  EXPECT_EQ(features::alarm_next_fire_epoch(), 86400 + 3600);
}

TEST(Alarm, StopDoesNotRefireSameMinute) {
  AppsFixture f;
  f.clock.set_epoch_s(1020);
  f.settings.tz_offset_min = 0;
  features::alarm_set(0, 0, 17, 0, true, f.ctx);
  features::kAlarm.tick(60'000, f.ctx);
  ASSERT_TRUE(features::alarm_ringing());
  f.act(features::kAlarm, ActionType::AlarmStop);
  EXPECT_FALSE(features::alarm_ringing());
  // 同じ分の再発火はしない (翌日の 00:17 が次回)
  EXPECT_EQ(features::alarm_next_fire_epoch(), 86400 + 1020);
  features::kAlarm.tick(61'000, f.ctx);
  EXPECT_FALSE(features::alarm_ringing());
}

TEST(Alarm, SnoozeFiresFiveMinutesLater) {
  AppsFixture f;
  f.clock.set_epoch_s(1020);
  f.settings.tz_offset_min = 0;
  features::alarm_set(0, 0, 17, 0, true, f.ctx);
  features::kAlarm.tick(60'000, f.ctx);
  ASSERT_TRUE(features::alarm_ringing());
  f.act(features::kAlarm, ActionType::AlarmSnooze);
  EXPECT_FALSE(features::alarm_ringing());
  EXPECT_EQ(features::alarm_next_fire_epoch(), 1020 + 300);
  f.clock.set_epoch_s(1020 + 300);
  features::kAlarm.tick(360'000, f.ctx);
  EXPECT_TRUE(features::alarm_ringing());
}

TEST(Alarm, PersistAndRestore) {
  test::MemoryKeyValueStore kv;
  {
    EventBus bus;
    test::FakeClock clock;
    Settings settings;
    FeatureContext ctx{bus, kv, clock, nullptr, nullptr};
    ctx.settings = &settings;
    features::alarm_reset_state();
    clock.set_epoch_s(1000);
    settings.tz_offset_min = 0;
    features::alarm_set(0, 7, 30, 0x7E, true, ctx);
    features::kAlarm.save(ctx);
  }
  {
    EventBus bus;
    test::FakeClock clock;
    Settings settings;
    FeatureContext ctx{bus, kv, clock, nullptr, nullptr};
    ctx.settings = &settings;
    features::alarm_reset_state();
    EXPECT_EQ(features::alarm_count(), 0u);
    clock.set_epoch_s(2000);
    features::kAlarm.restore(ctx);
    EXPECT_EQ(features::alarm_count(), 1u);
    AlarmEntry e;
    ASSERT_TRUE(features::alarm_at(0, &e));
    EXPECT_EQ(e.hour, 7);
    EXPECT_EQ(e.min, 30);
    EXPECT_EQ(e.dow, 0x7E);
    // 次回発火がスケジュールされている
    EXPECT_GT(features::alarm_next_fire_epoch(), 2000);
  }
}

TEST(Alarm, RingAutoStopsAfterTimeout) {
  AppsFixture f;
  f.clock.set_epoch_s(1020);
  f.clock.set_now_ms(0);
  f.settings.tz_offset_min = 0;
  features::alarm_set(0, 0, 17, 0, true, f.ctx);
  features::kAlarm.tick(0, f.ctx);
  ASSERT_TRUE(features::alarm_ringing());
  features::kAlarm.tick(features::kAlarmRingTimeoutMs + 1, f.ctx);
  EXPECT_FALSE(features::alarm_ringing());
}

TEST(Alarm, ToggleFlipsEnabled) {
  AppsFixture f;
  const int32_t id = features::alarm_set(0, 7, 0, 0, true, f.ctx);
  ASSERT_GT(id, 0);
  f.act(features::kAlarm, ActionType::AlarmToggle,
        static_cast<uint32_t>(id));
  AlarmEntry e;
  ASSERT_TRUE(features::alarm_find(static_cast<uint32_t>(id), &e));
  EXPECT_EQ(e.enabled, 0);
  EXPECT_EQ(features::alarm_next_fire_epoch(), 0);  // OFF なので発火なし
}

// ---------- Notify ----------

TEST(Notify, RingKeepsLatest20) {
  AppsFixture f;
  for (int i = 0; i < 25; ++i) {
    std::string t = "t" + std::to_string(i);
    features::notify_add("app", t.c_str(), "body");
  }
  EXPECT_EQ(features::notify_count(), features::kNotifyMax);
  NotifyEntry e;
  ASSERT_TRUE(features::notify_at(0, &e));  // 最新
  EXPECT_STREQ(e.title, "t24");
  ASSERT_TRUE(features::notify_at(19, &e));  // 最古 (残っている方)
  EXPECT_STREQ(e.title, "t5");
  EXPECT_FALSE(features::notify_at(20, &e));
}

TEST(Notify, ClearAll) {
  AppsFixture f;
  features::notify_add("a", "t", "b");
  features::notify_add("a", "t2", "b2");
  EXPECT_EQ(features::notify_count(), 2u);
  features::notify_clear(f.ctx);
  EXPECT_EQ(features::notify_count(), 0u);
  EXPECT_EQ(f.rec.count_of(EventType::NotificationsCleared), 1u);
}

// ---------- Media ----------

TEST(Media, StateAndCommand) {
  AppsFixture f;
  features::media_set("song", "artist", true, f.ctx);
  const MediaState& s = features::media_state();
  EXPECT_TRUE(s.valid);
  EXPECT_TRUE(s.playing);
  EXPECT_STREQ(s.title, "song");
  EXPECT_STREQ(s.artist, "artist");
  EXPECT_EQ(f.rec.count_of(EventType::MediaStateChanged), 1u);

  f.act(features::kMedia, ActionType::MediaCommand,
        static_cast<uint32_t>(MediaCmd::Next));
  const auto* e = f.rec.last_of(EventType::MediaCmdRequested);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->arg0, static_cast<uint32_t>(MediaCmd::Next));
}

// ---------- alarm.* dispatch ----------

namespace {

struct AlarmSvcFixture {
  EventBus bus;
  test::MemoryKeyValueStore kv;
  test::FakeClock clock;
  Settings settings;
  FeatureContext ctx{bus, kv, clock, nullptr, nullptr};
  proto::Services svc;

  AlarmSvcFixture() {
    ctx.settings = &settings;
    features::alarm_reset_state();
    svc.kv = &kv;
    svc.bus = &bus;
    svc.settings = &settings;
    svc.ctx = &ctx;
    svc.alarm_count = [](void* c) -> int32_t {
      return static_cast<int32_t>(
          features::alarm_count());
    };
    svc.alarm_entry = [](uint32_t i, cbor::Writer& w, void* c) -> bool {
      AlarmEntry e;
      if (!features::alarm_at(i, &e)) return false;
      w.map(5)
          .text("id")
          .uint_v(e.id)
          .text("hour")
          .uint_v(e.hour)
          .text("min")
          .uint_v(e.min)
          .text("dow")
          .uint_v(e.dow)
          .text("on")
          .bool_v(e.enabled != 0);
      return true;
    };
    svc.alarm_set = [](uint32_t id, uint8_t hour, uint8_t min, uint8_t dow,
                       bool on, void* c) -> int32_t {
      return features::alarm_set(id, hour, min, dow, on,
                                 *static_cast<FeatureContext*>(c));
    };
    svc.alarm_delete = [](uint32_t id, void* c) -> int32_t {
      return features::alarm_delete(id, *static_cast<FeatureContext*>(c));
    };
  }

  // {"m":method,"p":params} を CBOR 化して dispatch に投げる。
  proto::DispatchError call(
      const char* method, std::function<void(cbor::Writer&)> put_p,
      cbor::Writer* res) {
    uint8_t req_buf[256];
    cbor::Writer w(req_buf, sizeof(req_buf));
    w.map(2).text("m").text(method).text("p");
    if (put_p) put_p(w); else w.map(0);
    EXPECT_TRUE(w.ok());
    return proto::dispatch_req(req_buf, w.size(), svc, res);
  }
};

}  // namespace

TEST(AlarmProto, SetAndListAndDelete) {
  AlarmSvcFixture f;
  uint8_t res_buf[256];
  cbor::Writer res(res_buf, sizeof(res_buf));

  // alarm.set {hour:7,min:30} → {id:N}
  EXPECT_EQ(f.call("alarm.set",
                   [](cbor::Writer& w) {
                     w.map(2).text("hour").uint_v(7).text("min").uint_v(30);
                   },
                   &res),
            proto::DispatchError::Ok);
  cbor::Value top{res_buf, res_buf + res.size()};
  cbor::Value v;
  ASSERT_TRUE(cbor::map_find(top, "r", &v));
  cbor::Value idv;
  int64_t id = 0;
  ASSERT_TRUE(cbor::map_find(v, "id", &idv));
  ASSERT_TRUE(cbor::as_int(idv, &id));
  EXPECT_GT(id, 0);

  // alarm.list → {alarms:[{...}]}
  res = cbor::Writer(res_buf, sizeof(res_buf));
  EXPECT_EQ(f.call("alarm.list", nullptr, &res), proto::DispatchError::Ok);
  top = {res_buf, res_buf + res.size()};
  ASSERT_TRUE(cbor::map_find(top, "r", &v));
  cbor::Value arr;
  ASSERT_TRUE(cbor::map_find(v, "alarms", &arr));
  size_t n = 0;
  ASSERT_TRUE(cbor::container_count(arr, &n));
  EXPECT_EQ(n, 1u);
  cbor::Value e0;
  ASSERT_TRUE(cbor::array_at(arr, 0, &e0));
  cbor::Value hv;
  int64_t h = -1;
  ASSERT_TRUE(cbor::map_find(e0, "hour", &hv));
  ASSERT_TRUE(cbor::as_int(hv, &h));
  EXPECT_EQ(h, 7);

  // alarm.delete {id}
  res = cbor::Writer(res_buf, sizeof(res_buf));
  EXPECT_EQ(f.call("alarm.delete",
                   [&](cbor::Writer& w) {
                     w.map(1).text("id").uint_v(static_cast<uint32_t>(id));
                   },
                   &res),
            proto::DispatchError::Ok);
  EXPECT_EQ(features::alarm_count(), 0u);

  // もう一度消す → not_found
  res = cbor::Writer(res_buf, sizeof(res_buf));
  EXPECT_EQ(f.call("alarm.delete",
                   [&](cbor::Writer& w) {
                     w.map(1).text("id").uint_v(static_cast<uint32_t>(id));
                   },
                   &res),
            proto::DispatchError::NotFound);
}

TEST(AlarmProto, BadParams) {
  AlarmSvcFixture f;
  uint8_t res_buf[256];
  cbor::Writer res(res_buf, sizeof(res_buf));
  // hour 必須
  EXPECT_EQ(f.call("alarm.set",
                   [](cbor::Writer& w) { w.map(1).text("min").uint_v(30); },
                   &res),
            proto::DispatchError::BadRequest);
  // hour>23
  res = cbor::Writer(res_buf, sizeof(res_buf));
  EXPECT_EQ(f.call("alarm.set",
                   [](cbor::Writer& w) {
                     w.map(2).text("hour").uint_v(24).text("min").uint_v(0);
                   },
                   &res),
            proto::DispatchError::BadRequest);
  // 満杯 → busy
  for (int i = 0; i < 5; ++i) {
    features::alarm_set(0, 1, static_cast<uint8_t>(i), 0, true, f.ctx);
  }
  res = cbor::Writer(res_buf, sizeof(res_buf));
  EXPECT_EQ(f.call("alarm.set",
                   [](cbor::Writer& w) {
                     w.map(2).text("hour").uint_v(2).text("min").uint_v(0);
                   },
                   &res),
            proto::DispatchError::Busy);
}
