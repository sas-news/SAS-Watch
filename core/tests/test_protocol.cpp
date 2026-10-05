// Protocol (CRC16/CBOR/Frame/Dispatch) のホストテスト。
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "watch/features/memo.hpp"
#include "watch/features/timer.hpp"
#include "watch/protocol/dispatch.hpp"
#include "watch/protocol/frame.hpp"
#include "fakes.hpp"

using namespace watch;

// ---------- CRC16 ----------

TEST(Crc16, KnownVector) {
  // CRC-16/CCITT-FALSE("123456789") = 0x29B1
  const uint8_t s[] = "123456789";
  EXPECT_EQ(proto::crc16_ccitt_false(s, 9), 0x29B1);
}

TEST(Crc16, EmptyIsInit) {
  EXPECT_EQ(proto::crc16_ccitt_false(nullptr, 0), 0xFFFF);
}

// ---------- CBOR ----------

TEST(Cbor, RoundTripMap) {
  uint8_t buf[256];
  cbor::Writer w(buf, sizeof(buf));
  w.map(4)
      .text("i").int_v(-5)
      .text("u").uint_v(300)
      .text("b").bool_v(true)
      .text("t").text("hello");
  ASSERT_TRUE(w.ok());

  cbor::Value top{buf, buf + w.size()};
  ASSERT_EQ(cbor::type(top), cbor::Type::Map);

  cbor::Value v;
  int64_t i;
  uint64_t u;
  bool b;
  const char* s;
  size_t n;
  ASSERT_TRUE(cbor::map_find(top, "i", &v));
  ASSERT_TRUE(cbor::as_int(v, &i));
  EXPECT_EQ(i, -5);
  ASSERT_TRUE(cbor::map_find(top, "u", &v));
  ASSERT_TRUE(cbor::as_uint(v, &u));
  EXPECT_EQ(u, 300u);
  ASSERT_TRUE(cbor::map_find(top, "b", &v));
  ASSERT_TRUE(cbor::as_bool(v, &b));
  EXPECT_TRUE(b);
  ASSERT_TRUE(cbor::map_find(top, "t", &v));
  ASSERT_TRUE(cbor::as_text(v, &s, &n));
  EXPECT_EQ(std::string(s, n), "hello");
  EXPECT_FALSE(cbor::map_find(top, "missing", &v));
}

TEST(Cbor, ArrayAt) {
  uint8_t buf[64];
  cbor::Writer w(buf, sizeof(buf));
  w.array(3).uint_v(10).uint_v(20).uint_v(30);
  ASSERT_TRUE(w.ok());
  cbor::Value arr{buf, buf + w.size()};
  cbor::Value v;
  uint64_t u;
  ASSERT_TRUE(cbor::array_at(arr, 2, &v));
  ASSERT_TRUE(cbor::as_uint(v, &u));
  EXPECT_EQ(u, 30u);
  EXPECT_FALSE(cbor::array_at(arr, 3, &v));
}

TEST(Cbor, MalformedRejected) {
  uint8_t bad[1] = {0xFF};  // break/stop code → 値ではない
  cbor::Value v{bad, bad + 1};
  EXPECT_EQ(cbor::type(v), cbor::Type::Invalid);
  cbor::Value out;
  EXPECT_FALSE(cbor::map_find(v, "x", &out));
  // indefinite-length は扱わない: map の中身が読めず false。
  uint8_t indef[1] = {0xBF};
  cbor::Value vi{indef, indef + 1};
  EXPECT_FALSE(cbor::map_find(vi, "x", &out));
  // 切り詰められた map(1): キーも値も足りない。
  uint8_t trunc_map[1] = {0xA1};
  cbor::Value tm{trunc_map, trunc_map + 1};
  EXPECT_FALSE(cbor::map_find(tm, "x", &out));
  // 長さだけの切り詰めデータ。
  uint8_t t2[2] = {0x65, 'a'};  // text(5) だがデータ無し
  cbor::Value v2{t2, t2 + 2};
  const char* s;
  size_t n;
  EXPECT_FALSE(cbor::as_text(v2, &s, &n));
}

// ---------- Frame ----------

TEST(Frame, RoundTrip) {
  const uint8_t payload[] = {0xA1, 0x61, 0x6D, 0x65, 0x68, 0x65, 0x6C, 0x6C};
  uint8_t buf[128];
  const size_t n = proto::frame_encode(proto::FrameType::Req, false, 0, 0x1234,
                                       payload, sizeof(payload), buf,
                                       sizeof(buf));
  ASSERT_GT(n, 0u);
  proto::Frame f;
  ASSERT_EQ(proto::frame_parse(buf, n, &f), proto::FrameError::Ok);
  EXPECT_EQ(f.type, proto::FrameType::Req);
  EXPECT_EQ(f.msg_id, 0x1234);
  EXPECT_FALSE(f.more);
  EXPECT_EQ(f.payload_len, sizeof(payload));
  EXPECT_EQ(std::memcmp(f.payload, payload, sizeof(payload)), 0);
}

TEST(Frame, BadCrcRejected) {
  const uint8_t payload[] = {1, 2, 3};
  uint8_t buf[64];
  const size_t n = proto::frame_encode(proto::FrameType::Evt, false, 0, 7,
                                       payload, sizeof(payload), buf,
                                       sizeof(buf));
  buf[n - 1] ^= 0xFF;  // CRC を壊す
  proto::Frame f;
  EXPECT_EQ(proto::frame_parse(buf, n, &f), proto::FrameError::BadCrc);
}

TEST(Frame, BadVersionRejected) {
  const uint8_t payload[] = {1};
  uint8_t buf[64];
  const size_t n = proto::frame_encode(proto::FrameType::Evt, false, 0, 7,
                                       payload, sizeof(payload), buf,
                                       sizeof(buf));
  buf[0] = 99;
  // CRC を直さないと CRC エラーになるので version チェックが先に来ること。
  proto::Frame f;
  EXPECT_EQ(proto::frame_parse(buf, n, &f), proto::FrameError::BadVersion);
}

TEST(Frame, FragmentsReassembled) {
  // 400バイトの payload を MTU 100 で送る。
  std::vector<uint8_t> payload(400);
  for (size_t i = 0; i < payload.size(); ++i) payload[i] = i & 0xFF;
  std::vector<std::vector<uint8_t>> frames;
  auto emit = [](const uint8_t* fr, size_t len, void* ctx) {
    static_cast<std::vector<std::vector<uint8_t>>*>(ctx)->emplace_back(fr,
                                                                      fr + len);
    return true;
  };
  ASSERT_TRUE(proto::send_message(proto::FrameType::Req, 0x42, payload.data(),
                                  payload.size(), 100, emit, &frames));
  ASSERT_GT(frames.size(), 1u);

  proto::Reassembler r;
  proto::Frame out;
  bool done = false;
  for (auto& fb : frames) {
    proto::Frame f;
    ASSERT_EQ(proto::frame_parse(fb.data(), fb.size(), &f),
              proto::FrameError::Ok);
    done = r.feed(f, &out);
  }
  ASSERT_TRUE(done);
  EXPECT_EQ(out.payload_len, payload.size());
  EXPECT_EQ(std::memcmp(out.payload, payload.data(), payload.size()), 0);
  EXPECT_EQ(out.msg_id, 0x42);
}

TEST(Frame, SeqGapIsError) {
  proto::Reassembler r;
  const uint8_t p[] = {1, 2};
  proto::Frame f0{proto::FrameType::Req, true, 0, 1, p, 2};
  proto::Frame f1{proto::FrameType::Req, false, 2, 1, p, 2};  // seq 飛び
  proto::Frame out;
  EXPECT_FALSE(r.feed(f0, &out));
  EXPECT_FALSE(r.feed(f1, &out));
  EXPECT_TRUE(r.error());
}

TEST(Frame, OversizeRejected) {
  proto::Reassembler r;
  uint8_t big[600] = {};
  proto::Frame f0{proto::FrameType::Req, true, 0, 1, big, sizeof(big)};
  proto::Frame f1{proto::FrameType::Req, false, 1, 1, big, sizeof(big)};
  proto::Frame out;
  r.feed(f0, &out);
  r.feed(f1, &out);  // 1200 > kMaxMessage(1024) → error
  EXPECT_TRUE(r.error());
}

// ---------- Dispatch ----------

namespace {

struct SvcFixture {
  test::FakeClock clock;
  test::MemoryKeyValueStore kv;
  Settings settings;
  EventBus bus;
  PowerPolicy power{&bus};
  InputMapper input;
  FeatureContext fctx{bus, kv, clock, nullptr, &power};
  proto::Services svc;
  test::EventRecorder rec;

  int battery = 87;
  bool charging = true;
  int timer_started_s = -1;
  int timer_stopped = 0;
  std::string notified_app;
  std::string media_title;

  SvcFixture() {
    rec.attach_all(bus);
    features::timer_reset_state();
    features::memo_reset_state();
    svc.clock = &clock;
    svc.kv = &kv;
    svc.settings = &settings;
    svc.bus = &bus;
    svc.power = &power;
    svc.input = &input;
    svc.ctx = this;
    svc.battery_percent = [](void* c) {
      return static_cast<SvcFixture*>(c)->battery;
    };
    svc.is_charging = [](void* c) {
      return static_cast<SvcFixture*>(c)->charging;
    };
    svc.timer_start = [](uint32_t s, void* c) {
      static_cast<SvcFixture*>(c)->timer_started_s = static_cast<int>(s);
      Action a;
      a.type = ActionType::TimerStart;
      a.arg0 = s;
      return features::kTimer.handle(a, static_cast<SvcFixture*>(c)->fctx);
    };
    svc.timer_stop = [](void* c) {
      ++static_cast<SvcFixture*>(c)->timer_stopped;
      Action a;
      a.type = ActionType::TimerStop;
      return features::kTimer.handle(a, static_cast<SvcFixture*>(c)->fctx);
    };
    svc.memo_create = [](const char* t, size_t n, void* c) {
      return features::memo_create(t, n, static_cast<SvcFixture*>(c)->fctx);
    };
    svc.notify_posted = [](const char* app, const char*, const char*, void* c) {
      static_cast<SvcFixture*>(c)->notified_app = app;
    };
    svc.media_state = [](const char* title, const char*, bool, void* c) {
      static_cast<SvcFixture*>(c)->media_title = title;
    };
  }

  // {"m":name,"p":{...}} をエンコードして dispatch する。
  size_t req(const char* method, void (*write_p)(cbor::Writer&), uint8_t* out,
             size_t cap) {
    uint8_t buf[512];
    cbor::Writer w(buf, sizeof(buf));
    w.map(2).text("m").text(method).text("p");
    if (write_p) {
      write_p(w);
    } else {
      w.map(0);
    }
    EXPECT_TRUE(w.ok());
    cbor::Writer ow(out, cap);
    return proto::dispatch_req(buf, w.size(), svc, &ow) ==
                   proto::DispatchError::Ok
               ? ow.size()
               : ow.size();
  }

  bool res_ok(const uint8_t* res, size_t n) {
    cbor::Value top{res, res + n}, v;
    bool ok = false;
    return cbor::map_find(top, "ok", &v) && cbor::as_bool(v, &ok) && ok;
  }
  bool res_err_code(const uint8_t* res, size_t n, const char** code,
                    size_t* code_n) {
    cbor::Value top{res, res + n}, v;
    bool ok = true;
    if (!cbor::map_find(top, "ok", &v) || !cbor::as_bool(v, &ok) || ok) {
      return false;
    }
    if (!cbor::map_find(top, "e", &v)) return false;
    return cbor::as_text(v, code, code_n);
  }
};

}  // namespace

TEST(Dispatch, Hello) {
  SvcFixture f;
  uint8_t out[512];
  size_t n = f.req(
      "hello",
      [](cbor::Writer& w) {
        w.map(3).text("proto").uint_v(1).text("app").text("0.1.0").text("os").text("android");
      },
      out, sizeof(out));
  EXPECT_TRUE(f.res_ok(out, n));
  cbor::Value top{out, out + n}, r, v;
  ASSERT_TRUE(cbor::map_find(top, "r", &r));
  ASSERT_TRUE(cbor::map_find(r, "proto", &v));
  int64_t p;
  ASSERT_TRUE(cbor::as_int(v, &p));
  EXPECT_EQ(p, 1);
}

TEST(Dispatch, HelloWrongProto) {
  SvcFixture f;
  uint8_t out[512];
  f.req(
      "hello", [](cbor::Writer& w) { w.map(1).text("proto").uint_v(2); }, out,
      sizeof(out));
  const char* code = nullptr;
  size_t cn = 0;
  cbor::Value v{out, out + 512};
  // res のサイズは req 内で把握していないので buf 全体で parse。
  ASSERT_TRUE(f.res_err_code(out, 512, &code, &cn));
  EXPECT_EQ(std::string(code, cn), "unsupported_proto");
}

TEST(Dispatch, UnknownMethod) {
  SvcFixture f;
  uint8_t out[512];
  f.req("no.such.method", nullptr, out, sizeof(out));
  const char* code;
  size_t cn;
  ASSERT_TRUE(f.res_err_code(out, 512, &code, &cn));
  EXPECT_EQ(std::string(code, cn), "unknown_method");
}

TEST(Dispatch, GarbageIsBadRequest) {
  SvcFixture f;
  const uint8_t junk[] = {0xFF, 0x01};
  uint8_t out[64];
  cbor::Writer ow(out, sizeof(out));
  EXPECT_EQ(proto::dispatch_req(junk, sizeof(junk), f.svc, &ow),
            proto::DispatchError::BadRequest);
  const char* code;
  size_t cn;
  ASSERT_TRUE(f.res_err_code(out, ow.size(), &code, &cn));
  EXPECT_EQ(std::string(code, cn), "bad_request");
}

TEST(Dispatch, TimeSet) {
  SvcFixture f;
  uint8_t out[512];
  f.req(
      "time.set",
      [](cbor::Writer& w) {
        w.map(2).text("epoch").int_v(1'700'000'500).text("tz_offset_min").int_v(540);
      },
      out, sizeof(out));
  EXPECT_EQ(f.clock.epoch_s(), 1'700'000'500);
  EXPECT_EQ(f.settings.tz_offset_min, 540);
}

TEST(Dispatch, DeviceInfo) {
  SvcFixture f;
  uint8_t out[512];
  size_t n = f.req("device.info", nullptr, out, sizeof(out));
  cbor::Value top{out, out + n}, r, v;
  ASSERT_TRUE(cbor::map_find(top, "r", &r));
  ASSERT_TRUE(cbor::map_find(r, "battery", &v));
  int64_t b;
  ASSERT_TRUE(cbor::as_int(v, &b));
  EXPECT_EQ(b, 87);
  ASSERT_TRUE(cbor::map_find(r, "charging", &v));
  bool c;
  ASSERT_TRUE(cbor::as_bool(v, &c));
  EXPECT_TRUE(c);
}

TEST(Dispatch, SettingsGetAll) {
  SvcFixture f;
  uint8_t out[1024];
  size_t n = f.req("settings.get", nullptr, out, sizeof(out));
  cbor::Value top{out, out + n}, r, v;
  ASSERT_TRUE(cbor::map_find(top, "r", &r));
  ASSERT_TRUE(cbor::map_find(r, "brightness", &v));
  int64_t b;
  ASSERT_TRUE(cbor::as_int(v, &b));
  EXPECT_EQ(b, 50);
  ASSERT_TRUE(cbor::map_find(r, "theme", &v));
  const char* s;
  size_t sn;
  ASSERT_TRUE(cbor::as_text(v, &s, &sn));
  EXPECT_EQ(std::string(s, sn), "standard");
}

TEST(Dispatch, SettingsGetSubset) {
  SvcFixture f;
  uint8_t out[1024];
  f.req(
      "settings.get",
      [](cbor::Writer& w) {
        w.map(1).text("keys").array(2).text("theme").text("unknown.key");
      },
      out, sizeof(out));
  cbor::Value top{out, out + 512}, r, v;
  ASSERT_TRUE(cbor::map_find(top, "r", &r));
  // 有効な theme だけ入る。
  EXPECT_TRUE(cbor::map_find(r, "theme", &v));
  EXPECT_FALSE(cbor::map_find(r, "unknown.key", &v));
}

TEST(Dispatch, SettingsSetAppliesSideEffects) {
  SvcFixture f;
  uint8_t out[512];
  size_t n = f.req(
      "settings.set",
      [](cbor::Writer& w) {
        w.map(3)
            .text("dim_after_s")
            .uint_v(20)
            .text("screen_off_after_s")
            .uint_v(40)
            .text("button.boot.short")
            .text("nav.quick");
      },
      out, sizeof(out));
  ASSERT_TRUE(f.res_ok(out, n));
  // PowerPolicy に反映される
  EXPECT_EQ(f.power.thresholds().dim_after_s, 20u);
  EXPECT_EQ(f.power.thresholds().screen_off_after_s, 40u);
  // InputMapper に反映される
  EXPECT_STREQ(f.input.action_name(PhysicalButton::Boot, PressType::Short),
               "nav.quick");
  // KV に永続化される
  Settings s2;
  settings_load(s2, f.kv);
  EXPECT_EQ(s2.dim_after_s, 20u);
}

TEST(Dispatch, TimerStartStop) {
  SvcFixture f;
  uint8_t out[512];
  f.req(
      "timer.start",
      [](cbor::Writer& w) { w.map(1).text("seconds").uint_v(120); },
      out, sizeof(out));
  EXPECT_EQ(f.timer_started_s, 120);
  EXPECT_TRUE(features::timer_state().running);
  f.req("timer.stop", nullptr, out, sizeof(out));
  EXPECT_EQ(f.timer_stopped, 1);
  EXPECT_FALSE(features::timer_state().running);
}

TEST(Dispatch, TimerStartBadParam) {
  SvcFixture f;
  uint8_t out[512];
  f.req(
      "timer.start",
      [](cbor::Writer& w) { w.map(1).text("seconds").uint_v(0); },
      out, sizeof(out));
  const char* code;
  size_t cn;
  ASSERT_TRUE(f.res_err_code(out, 512, &code, &cn));
  EXPECT_EQ(std::string(code, cn), "bad_request");
}

TEST(Dispatch, MemoCreateReturnsId) {
  SvcFixture f;
  uint8_t out[512];
  size_t n = f.req(
      "memo.create",
      [](cbor::Writer& w) { w.map(1).text("text").text("買い物リスト"); },
      out, sizeof(out));
  ASSERT_TRUE(f.res_ok(out, n));
  cbor::Value top{out, out + n}, r, v;
  ASSERT_TRUE(cbor::map_find(top, "r", &r));
  ASSERT_TRUE(cbor::map_find(r, "id", &v));
  int64_t id;
  ASSERT_TRUE(cbor::as_int(v, &id));
  EXPECT_GT(id, 0);
  EXPECT_EQ(features::memo_count(), 1u);
}

TEST(Dispatch, NotifyPostCallsHandler) {
  SvcFixture f;
  uint8_t out[512];
  f.req(
      "notify.post",
      [](cbor::Writer& w) {
        w.map(3).text("app").text("LINE").text("title").text("t").text("body").text("b");
      },
      out, sizeof(out));
  EXPECT_EQ(f.notified_app, "LINE");
  EXPECT_EQ(f.rec.count_of(EventType::NotificationPosted), 1u);
}

TEST(Dispatch, MediaStateCallsHandler) {
  SvcFixture f;
  uint8_t out[512];
  f.req(
      "media.state",
      [](cbor::Writer& w) {
        w.map(3).text("title").text("Song").text("artist").text("Artist").text("playing").bool_v(true);
      },
      out, sizeof(out));
  EXPECT_EQ(f.media_title, "Song");
}
