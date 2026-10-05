// Protocol (CRC16/CBOR/Frame/Dispatch) のホストテスト。
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#include "watch/features/memo.hpp"
#include "watch/features/timer.hpp"
#include "watch/protocol/dispatch.hpp"
#include "watch/protocol/frame.hpp"
#include "watch/protocol/sha256.hpp"
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
  template <typename F>
  size_t req(const char* method, F&& write_p, uint8_t* out, size_t cap) {
    uint8_t buf[512];
    cbor::Writer w(buf, sizeof(buf));
    w.map(2).text("m").text(method).text("p");
    if constexpr (std::is_same_v<std::decay_t<F>, std::nullptr_t>) {
      w.map(0);
    } else {
      write_p(w);
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

// ---------- Dispatch: memo.* (Phase 9) ----------

namespace {

struct SvcMemoFixture : SvcFixture {
  test::FakeAudio audio;
  uint32_t audio_sent = 0;

  SvcMemoFixture() {
    audio.set_clock(&clock);
    fctx.audio = &audio;
    fctx.settings = &settings;
    svc.memo_count = [](void*) {
      return static_cast<int32_t>(features::memo_count());
    };
    svc.memo_entry = [](uint32_t i, cbor::Writer& w, void*) {
      const int32_t total = static_cast<int32_t>(features::memo_count());
      features::MemoEntry e;
      if (static_cast<int32_t>(i) >= total ||
          !features::memo_at(total - 1 - static_cast<int32_t>(i), &e)) {
        return false;
      }
      w.map(4)
          .text("id").uint_v(e.id)
          .text("kind").text(e.kind == features::MemoKind::Voice ? "voice" : "text")
          .text("sec").uint_v(e.sec)
          .text("size").uint_v(e.size);
      return true;
    };
    svc.memo_get = [](uint32_t id, cbor::Writer& w, void*) {
      features::MemoEntry e;
      if (!features::memo_find(id, &e)) return false;
      w.map(5)
          .text("id").uint_v(e.id)
          .text("kind").text(e.kind == features::MemoKind::Voice ? "voice" : "text")
          .text("sec").uint_v(e.sec)
          .text("size").uint_v(e.size)
          .text("text").text(e.text);
      return true;
    };
    svc.memo_delete = [](uint32_t id, void* c) {
      return features::memo_delete(id, static_cast<SvcMemoFixture*>(c)->fctx)
                 ? 1
                 : 0;
    };
    svc.memo_audio_info = [](uint32_t id, uint32_t* size, uint8_t sha[32],
                             void* c) {
      auto* f = static_cast<SvcMemoFixture*>(c);
      features::MemoEntry e;
      if (!features::memo_find(id, &e) ||
          e.kind != features::MemoKind::Voice ||
          !f->audio.memo_audio_size(id, size)) {
        return false;
      }
      proto::Sha256 h;
      uint8_t buf[256];
      uint32_t off = 0;
      for (;;) {
        size_t len = sizeof(buf);
        if (!f->audio.memo_audio_read(id, off, buf, &len)) return false;
        if (len == 0) break;
        h.update(buf, len);
        off += len;
      }
      h.finish(sha);
      return true;
    };
    svc.memo_audio_send = [](uint32_t id, void* c) {
      static_cast<SvcMemoFixture*>(c)->audio_sent = id;
      return true;
    };
  }

  // 音声メモを 1 件録音→確定する。id を返す。
  uint32_t record_voice(uint32_t sec) {
    Action a{};
    a.type = ActionType::MemoRecordStart;
    features::kMemo.handle(a, fctx);
    audio.rec_elapsed_s = sec;
    features::kMemo.handle(a, fctx);
    const auto* ev = rec.last_of(EventType::MemoSaved);
    return ev ? ev->arg0 : 0;
  }
};

// res の r 内 key を取る補助。
bool res_r_find(const uint8_t* res, size_t n, const char* key, cbor::Value* out) {
  cbor::Value top{res, res + n}, r;
  return cbor::map_find(top, "r", &r) && cbor::map_find(r, key, out);
}

}  // namespace

TEST(DispatchMemo, ListEmpty) {
  SvcMemoFixture f;
  uint8_t out[512];
  const size_t n = f.req("memo.list", nullptr, out, sizeof(out));
  ASSERT_TRUE(f.res_ok(out, n));
  cbor::Value v;
  ASSERT_TRUE(res_r_find(out, n, "total", &v));
  int64_t total = -1;
  ASSERT_TRUE(cbor::as_int(v, &total));
  EXPECT_EQ(total, 0);
}

TEST(DispatchMemo, ListAndGet) {
  SvcMemoFixture f;
  features::memo_create("ichi", 4, f.fctx);
  const uint32_t vid = f.record_voice(7);
  ASSERT_GT(vid, 0u);

  uint8_t out[512];
  const size_t n = f.req(
      "memo.list",
      [](cbor::Writer& w) {
        w.map(2).text("i").uint_v(0).text("n").uint_v(8);
      },
      out, sizeof(out));
  ASSERT_TRUE(f.res_ok(out, n));
  cbor::Value v, arr, e0;
  ASSERT_TRUE(res_r_find(out, n, "total", &v));
  int64_t total = -1;
  ASSERT_TRUE(cbor::as_int(v, &total));
  EXPECT_EQ(total, 2);
  ASSERT_TRUE(res_r_find(out, n, "memos", &arr));
  ASSERT_TRUE(cbor::array_at(arr, 0, &e0));
  // 新しい順: 0 番目は直前に保存した音声メモ。
  cbor::Value kind, sec;
  ASSERT_TRUE(cbor::map_find(e0, "kind", &kind));
  const char* ks = nullptr;
  size_t kn = 0;
  ASSERT_TRUE(cbor::as_text(kind, &ks, &kn));
  EXPECT_EQ(std::string(ks, kn), "voice");
  ASSERT_TRUE(cbor::map_find(e0, "sec", &sec));
  int64_t s = -1;
  ASSERT_TRUE(cbor::as_int(sec, &s));
  EXPECT_EQ(s, 7);

  // memo.get でテキストメモの本文が取れる。
  const size_t gn = f.req(
      "memo.get",
      [](cbor::Writer& w) { w.map(1).text("id").uint_v(1); },
      out, sizeof(out));
  ASSERT_TRUE(f.res_ok(out, gn));
  cbor::Value t;
  ASSERT_TRUE(res_r_find(out, gn, "text", &t));
  const char* ts = nullptr;
  size_t tn = 0;
  ASSERT_TRUE(cbor::as_text(t, &ts, &tn));
  EXPECT_EQ(std::string(ts, tn), "ichi");
}

TEST(DispatchMemo, DeleteAndNotFound) {
  SvcMemoFixture f;
  const int32_t id = features::memo_create("x", 1, f.fctx);
  ASSERT_GT(id, 0);
  uint8_t out[512];
  const size_t n = f.req(
      "memo.delete",
      [id](cbor::Writer& w) {
        w.map(1).text("id").uint_v(static_cast<uint32_t>(id));
      },
      out, sizeof(out));
  ASSERT_TRUE(f.res_ok(out, n));
  const size_t n2 = f.req(
      "memo.delete",
      [id](cbor::Writer& w) {
        w.map(1).text("id").uint_v(static_cast<uint32_t>(id));
      },
      out, sizeof(out));
  const char* code = nullptr;
  size_t cn = 0;
  ASSERT_TRUE(f.res_err_code(out, n2, &code, &cn));
  EXPECT_EQ(std::string(code, cn), "not_found");
}

TEST(DispatchMemo, AudioGetReportsAndStartsSend) {
  SvcMemoFixture f;
  const uint32_t id = f.record_voice(3);
  ASSERT_GT(id, 0u);
  uint8_t out[512];
  const size_t n = f.req(
      "memo.audio.get",
      [id](cbor::Writer& w) { w.map(1).text("id").uint_v(id); },
      out, sizeof(out));
  ASSERT_TRUE(f.res_ok(out, n));
  cbor::Value v;
  ASSERT_TRUE(res_r_find(out, n, "size", &v));
  int64_t size = -1;
  ASSERT_TRUE(cbor::as_int(v, &size));
  EXPECT_EQ(size, static_cast<int64_t>(test::FakeAudio::kFileHeader +
                                       3 * test::FakeAudio::kBytesPerSec));
  ASSERT_TRUE(res_r_find(out, n, "sha256", &v));
  const uint8_t* sha = nullptr;
  size_t shn = 0;
  ASSERT_TRUE(cbor::as_bytes(v, &sha, &shn));
  EXPECT_EQ(shn, 32u);
  EXPECT_EQ(f.audio_sent, id);

  // 音声でないメモは not_found。
  const int32_t tid = features::memo_create("y", 1, f.fctx);
  const size_t n2 = f.req(
      "memo.audio.get",
      [tid](cbor::Writer& w) {
        w.map(1).text("id").uint_v(static_cast<uint32_t>(tid));
      },
      out, sizeof(out));
  const char* code = nullptr;
  size_t cn = 0;
  ASSERT_TRUE(f.res_err_code(out, n2, &code, &cn));
  EXPECT_EQ(std::string(code, cn), "not_found");
}

// ---------- Dispatch: wifi.* / ota.* (Phase OTA) ----------

namespace {

struct SvcOtaFixture : SvcFixture {
  std::string wifi_ssid;
  std::string wifi_pass;
  bool wifi_saved = false;
  std::string ota_url;
  std::string ota_version;
  uint8_t ota_sha[32] = {};
  int ota_start_ret = 0;
  bool ota_status_written = false;

  SvcOtaFixture() {
    svc.wifi_set = [](const char* s, const char* p, void* c) {
      auto* f = static_cast<SvcOtaFixture*>(c);
      f->wifi_ssid = s;
      f->wifi_pass = p;
      f->wifi_saved = true;
      return true;
    };
    svc.wifi_info = [](char* out, size_t cap, void* c) {
      auto* f = static_cast<SvcOtaFixture*>(c);
      if (!f->wifi_saved || f->wifi_ssid.size() >= cap) return false;
      std::strcpy(out, f->wifi_ssid.c_str());
      return true;
    };
    svc.ota_start = [](const char* url, const uint8_t sha[32], const char* ver,
                       void* c) {
      auto* f = static_cast<SvcOtaFixture*>(c);
      f->ota_url = url;
      f->ota_version = ver;
      std::memcpy(f->ota_sha, sha, sizeof(f->ota_sha));
      return f->ota_start_ret;
    };
    svc.ota_status = [](cbor::Writer& w, void* c) {
      auto* f = static_cast<SvcOtaFixture*>(c);
      f->ota_status_written = true;
      w.map(5)
          .text("active").bool_v(true)
          .text("stage").text("download")
          .text("pct").uint_v(42)
          .text("msg").text("")
          .text("version").text("0.2.0");
      return true;
    };
  }
};

}  // namespace

TEST(DispatchOta, WifiSetCallsHandler) {
  SvcOtaFixture f;
  uint8_t out[512];
  const size_t n = f.req(
      "wifi.set",
      [](cbor::Writer& w) {
        w.map(2).text("ssid").text("HomeWifi").text("pass").text("password123");
      },
      out, sizeof(out));
  ASSERT_TRUE(f.res_ok(out, n));
  EXPECT_EQ(f.wifi_ssid, "HomeWifi");
  EXPECT_EQ(f.wifi_pass, "password123");
}

TEST(DispatchOta, WifiSetRejectsBadParams) {
  SvcOtaFixture f;
  uint8_t out[512];
  const char* code = nullptr;
  size_t cn = 0;
  // ssid 空は bad_request
  f.req(
      "wifi.set",
      [](cbor::Writer& w) {
        w.map(2).text("ssid").text("").text("pass").text("password123");
      },
      out, sizeof(out));
  ASSERT_TRUE(f.res_err_code(out, 512, &code, &cn));
  EXPECT_EQ(std::string(code, cn), "bad_request");
  // pass 7文字 (WPA 未満) も bad_request
  f.req(
      "wifi.set",
      [](cbor::Writer& w) {
        w.map(2).text("ssid").text("Net").text("pass").text("1234567");
      },
      out, sizeof(out));
  ASSERT_TRUE(f.res_err_code(out, 512, &code, &cn));
  EXPECT_EQ(std::string(code, cn), "bad_request");
  EXPECT_FALSE(f.wifi_saved);
}

TEST(DispatchOta, WifiStatusReportsConfigured) {
  SvcOtaFixture f;
  uint8_t out[512];
  // 未設定: configured=false
  size_t n = f.req("wifi.status", nullptr, out, sizeof(out));
  cbor::Value v;
  ASSERT_TRUE(res_r_find(out, n, "configured", &v));
  bool conf = true;
  ASSERT_TRUE(cbor::as_bool(v, &conf));
  EXPECT_FALSE(conf);
  // wifi.set → wifi.status で ssid が見える (pass は含まれない)
  f.req(
      "wifi.set",
      [](cbor::Writer& w) {
        w.map(2).text("ssid").text("HomeWifi").text("pass").text("password123");
      },
      out, sizeof(out));
  n = f.req("wifi.status", nullptr, out, sizeof(out));
  ASSERT_TRUE(res_r_find(out, n, "configured", &v));
  ASSERT_TRUE(cbor::as_bool(v, &conf));
  EXPECT_TRUE(conf);
  ASSERT_TRUE(res_r_find(out, n, "ssid", &v));
  const char* s = nullptr;
  size_t sn = 0;
  ASSERT_TRUE(cbor::as_text(v, &s, &sn));
  EXPECT_EQ(std::string(s, sn), "HomeWifi");
  EXPECT_FALSE(res_r_find(out, n, "pass", &v));
}

TEST(DispatchOta, OtaStartCallsHandler) {
  SvcOtaFixture f;
  uint8_t out[512];
  const uint8_t sha[32] = {0xAB};
  const size_t n = f.req(
      "ota.start",
      [sha](cbor::Writer& w) {
        w.map(3)
            .text("url").text("https://example.com/fw.bin")
            .text("sha256").bytes(sha, sizeof(sha))
            .text("version").text("0.2.0");
      },
      out, sizeof(out));
  ASSERT_TRUE(f.res_ok(out, n));
  EXPECT_EQ(f.ota_url, "https://example.com/fw.bin");
  EXPECT_EQ(f.ota_version, "0.2.0");
  EXPECT_EQ(f.ota_sha[0], 0xAB);
}

TEST(DispatchOta, OtaStartBusyAndBadRequest) {
  SvcOtaFixture f;
  uint8_t out[512];
  const char* code = nullptr;
  size_t cn = 0;
  f.ota_start_ret = 1;  // 更新中
  const uint8_t sha[32] = {};
  f.req(
      "ota.start",
      [sha](cbor::Writer& w) {
        w.map(3)
            .text("url").text("https://example.com/fw.bin")
            .text("sha256").bytes(sha, sizeof(sha))
            .text("version").text("0.2.0");
      },
      out, sizeof(out));
  ASSERT_TRUE(f.res_err_code(out, 512, &code, &cn));
  EXPECT_EQ(std::string(code, cn), "busy");
  // sha256 が bytes でない → bad_request
  f.req(
      "ota.start",
      [](cbor::Writer& w) {
        w.map(3)
            .text("url").text("https://example.com/fw.bin")
            .text("sha256").text("abc")
            .text("version").text("0.2.0");
      },
      out, sizeof(out));
  ASSERT_TRUE(f.res_err_code(out, 512, &code, &cn));
  EXPECT_EQ(std::string(code, cn), "bad_request");
}

TEST(DispatchOta, OtaStatusWritesMap) {
  SvcOtaFixture f;
  uint8_t out[512];
  const size_t n = f.req("ota.status", nullptr, out, sizeof(out));
  ASSERT_TRUE(f.res_ok(out, n));
  EXPECT_TRUE(f.ota_status_written);
  cbor::Value v;
  ASSERT_TRUE(res_r_find(out, n, "pct", &v));
  int64_t pct = -1;
  ASSERT_TRUE(cbor::as_int(v, &pct));
  EXPECT_EQ(pct, 42);
  ASSERT_TRUE(res_r_find(out, n, "stage", &v));
  const char* s = nullptr;
  size_t sn = 0;
  ASSERT_TRUE(cbor::as_text(v, &s, &sn));
  EXPECT_EQ(std::string(s, sn), "download");
}
