// BULK 転送 (bulk.hpp) と SHA-256 のホストテスト。
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "watch/protocol/bulk.hpp"
#include "watch/protocol/cbor.hpp"

using namespace watch;

namespace {

// hex 文字列 → bytes
void hex_to_bytes(const char* hex, uint8_t* out, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    unsigned v;
    sscanf(hex + i * 2, "%2x", &v);
    out[i] = static_cast<uint8_t>(v);
  }
}

struct SinkState {
  std::vector<uint8_t> data;   // offset 位置に書き込んだ内容
  int begin_calls = 0;
  int commit_calls = 0;
  int abort_calls = 0;
  bool begin_ok = true;
  bool write_ok = true;
  bool commit_ok = true;

  static bool begin(uint16_t, const char*, uint32_t size, void* ctx) {
    auto* s = static_cast<SinkState*>(ctx);
    ++s->begin_calls;
    s->data.assign(size, 0);
    return s->begin_ok;
  }
  static bool write(uint16_t, uint32_t offset, const uint8_t* d, size_t len,
                    void* ctx) {
    auto* s = static_cast<SinkState*>(ctx);
    if (!s->write_ok) return false;
    if (offset + len > s->data.size()) s->data.resize(offset + len);
    std::memcpy(s->data.data() + offset, d, len);
    return true;
  }
  static bool commit(uint16_t, void* ctx) {
    auto* s = static_cast<SinkState*>(ctx);
    ++s->commit_calls;
    return s->commit_ok;
  }
  static void abort(uint16_t, void* ctx) {
    ++static_cast<SinkState*>(ctx)->abort_calls;
  }
};

proto::BulkReceiver::Sink make_sink(SinkState& s) {
  proto::BulkReceiver::Sink sink;
  sink.begin = &SinkState::begin;
  sink.write = &SinkState::write;
  sink.commit = &SinkState::commit;
  sink.abort = &SinkState::abort;
  sink.ctx = &s;
  return sink;
}

// BULK_START payload を Writer で作る。
size_t make_start(uint16_t id, const char* kind, uint32_t size,
                  const uint8_t sha[32], uint8_t* out, size_t cap) {
  cbor::Writer w(out, cap);
  w.map(5)
      .text("id")
      .uint_v(id)
      .text("kind")
      .text(kind)
      .text("size")
      .uint_v(size)
      .text("sha256")
      .bytes(sha, 32)
      .text("chunk")
      .uint_v(244);
  return w.ok() ? w.size() : 0;
}

// BULK_CHUNK payload (id:u16|offset:u32|bytes)。
std::vector<uint8_t> make_chunk(uint16_t id, uint32_t offset,
                                const uint8_t* data, size_t len) {
  std::vector<uint8_t> p(6 + len);
  p[0] = id & 0xFF;
  p[1] = id >> 8;
  p[2] = offset & 0xFF;
  p[3] = (offset >> 8) & 0xFF;
  p[4] = (offset >> 16) & 0xFF;
  p[5] = (offset >> 24) & 0xFF;
  std::memcpy(p.data() + 6, data, len);
  return p;
}

}  // namespace

// ---------- SHA-256 ----------

TEST(Sha256, KnownVectors) {
  uint8_t out[32];
  proto::sha256(reinterpret_cast<const uint8_t*>(""), 0, out);
  uint8_t want[32];
  hex_to_bytes(
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
      want, 32);
  EXPECT_EQ(std::memcmp(out, want, 32), 0);

  proto::sha256(reinterpret_cast<const uint8_t*>("abc"), 3, out);
  hex_to_bytes(
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
      want, 32);
  EXPECT_EQ(std::memcmp(out, want, 32), 0);
}

TEST(Sha256, StreamingMatchesOneShot) {
  std::vector<uint8_t> data(1000);
  for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i);
  uint8_t one[32], many[32];
  proto::sha256(data.data(), data.size(), one);

  proto::Sha256 s;
  for (size_t off = 0; off < data.size(); off += 7) {
    s.update(data.data() + off, std::min<size_t>(7, data.size() - off));
  }
  s.finish(many);
  EXPECT_EQ(std::memcmp(one, many, 32), 0);
}

// ---------- BULK_START/ACK エンコード/パース ----------

TEST(Bulk, StartParseRoundTrip) {
  uint8_t sha[32];
  for (int i = 0; i < 32; ++i) sha[i] = static_cast<uint8_t>(i);
  uint8_t buf[128];
  const size_t n = make_start(0x1234, "theme", 4096, sha, buf, sizeof(buf));
  ASSERT_GT(n, 0u);

  proto::BulkStart s;
  ASSERT_TRUE(proto::bulk_parse_start(buf, n, &s));
  EXPECT_EQ(s.id, 0x1234);
  EXPECT_STREQ(s.kind, "theme");
  EXPECT_EQ(s.size, 4096u);
  EXPECT_EQ(std::memcmp(s.sha256, sha, 32), 0);
  EXPECT_EQ(s.chunk, 244u);
}

TEST(Bulk, StartParseRejectsBad) {
  EXPECT_FALSE(proto::bulk_parse_start(nullptr, 0, nullptr));
  uint8_t bad[] = {0x01, 0x02};  // map ではない
  proto::BulkStart s;
  EXPECT_FALSE(proto::bulk_parse_start(bad, sizeof(bad), &s));

  // sha256 が 16B しかない
  uint8_t buf[64];
  cbor::Writer w(buf, sizeof(buf));
  w.map(2).text("id").uint_v(1).text("sha256").bytes(bad, 2);
  EXPECT_FALSE(proto::bulk_parse_start(buf, w.size(), &s));
}

TEST(Bulk, AckEncode) {
  uint8_t buf[32];
  const size_t n = proto::bulk_encode_ack(7, 1024, buf, sizeof(buf));
  ASSERT_GT(n, 0u);
  cbor::Value top{buf, buf + n};
  cbor::Value v;
  uint64_t u;
  ASSERT_TRUE(cbor::map_find(top, "id", &v));
  ASSERT_TRUE(cbor::as_uint(v, &u));
  EXPECT_EQ(u, 7u);
  ASSERT_TRUE(cbor::map_find(top, "next", &v));
  ASSERT_TRUE(cbor::as_uint(v, &u));
  EXPECT_EQ(u, 1024u);
}

// ---------- BulkReceiver ----------

TEST(BulkReceiver, HappyPath) {
  SinkState st;
  proto::BulkReceiver rx;
  rx.init(make_sink(st));

  // 64B のデータを送る
  std::vector<uint8_t> payload(64);
  for (size_t i = 0; i < payload.size(); ++i) payload[i] = i & 0xFF;
  uint8_t sha[32];
  proto::sha256(payload.data(), payload.size(), sha);

  proto::BulkStart start{/*.id =*/7, "asset", /*.size =*/64, {0}, 128};
  std::memcpy(start.sha256, sha, 32);

  uint32_t next = 0xFFFF;
  ASSERT_TRUE(rx.start(start, &next));
  EXPECT_EQ(next, 0u);
  EXPECT_TRUE(rx.in_progress());
  EXPECT_EQ(st.begin_calls, 1);

  bool ack = false;
  // 16B ずつ 4 チャンク
  for (uint32_t off = 0; off < 64; off += 16) {
    auto c = make_chunk(7, off, payload.data() + off, 16);
    ASSERT_TRUE(rx.feed_chunk(c.data(), c.size(), &next, &ack));
  }
  EXPECT_FALSE(ack);  // 4 チャンクではまだ ACK 不要
  EXPECT_EQ(rx.next_offset(), 64u);

  uint8_t end_buf[16];
  cbor::Writer w(end_buf, sizeof(end_buf));
  w.map(1).text("id").uint_v(7);
  uint16_t end_id;
  ASSERT_TRUE(proto::bulk_parse_end(end_buf, w.size(), &end_id));
  EXPECT_EQ(end_id, 7);

  ASSERT_TRUE(rx.finish(end_id));
  EXPECT_FALSE(rx.in_progress());
  EXPECT_EQ(st.commit_calls, 1);
  EXPECT_EQ(st.data, payload);
}

TEST(BulkReceiver, AckEveryEightChunks) {
  SinkState st;
  proto::BulkReceiver rx;
  rx.init(make_sink(st));

  std::vector<uint8_t> payload(16);
  uint8_t sha[32];
  proto::sha256(payload.data(), payload.size(), sha);
  proto::BulkStart start{9, "asset", 16, {0}, 8};
  std::memcpy(start.sha256, sha, 32);
  ASSERT_TRUE(rx.start(start, nullptr));

  uint32_t next = 0;
  bool ack = false;
  // 2B × 8 でちょうど 16B。8個目で ACK が要る。
  for (uint32_t off = 0; off < 16; off += 2) {
    auto c = make_chunk(9, off, payload.data() + off, 2);
    ASSERT_TRUE(rx.feed_chunk(c.data(), c.size(), &next, &ack));
  }
  EXPECT_TRUE(ack);
  EXPECT_EQ(next, 16u);
}

TEST(BulkReceiver, ResumeAfterReconnect) {
  SinkState st;
  proto::BulkReceiver rx;
  rx.init(make_sink(st));

  std::vector<uint8_t> payload(32);
  for (size_t i = 0; i < payload.size(); ++i) payload[i] = i;
  uint8_t sha[32];
  proto::sha256(payload.data(), payload.size(), sha);
  proto::BulkStart start{3, "ota", 32, {0}, 8};
  std::memcpy(start.sha256, sha, 32);
  ASSERT_TRUE(rx.start(start, nullptr));

  uint32_t next = 0;
  bool ack = false;
  auto c1 = make_chunk(3, 0, payload.data(), 16);
  ASSERT_TRUE(rx.feed_chunk(c1.data(), c1.size(), &next, &ack));

  // --- 切断を模擬 (reset しない) ---
  // Phone が同じ id・同じ内容で BULK_START を再送 → 途中位置を返す。
  ASSERT_TRUE(rx.start(start, &next));
  EXPECT_EQ(next, 16u);
  EXPECT_EQ(st.begin_calls, 1);  // 再開では begin を呼ばない

  auto c2 = make_chunk(3, 16, payload.data() + 16, 16);
  ASSERT_TRUE(rx.feed_chunk(c2.data(), c2.size(), &next, &ack));
  ASSERT_TRUE(rx.finish(3));
  EXPECT_EQ(st.commit_calls, 1);
  EXPECT_EQ(st.data, payload);
}

TEST(BulkReceiver, WrongOffsetTriggersAck) {
  SinkState st;
  proto::BulkReceiver rx;
  rx.init(make_sink(st));
  uint8_t sha[32] = {0};
  proto::BulkStart start{5, "asset", 64, {0}, 8};
  std::memcpy(start.sha256, sha, 32);
  ASSERT_TRUE(rx.start(start, nullptr));

  uint8_t d[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  uint32_t next = 0;
  bool ack = false;
  // offset 32 のチャンクが先に来た → ACK{next=0} を要求
  auto c = make_chunk(5, 32, d, 8);
  ASSERT_TRUE(rx.feed_chunk(c.data(), c.size(), &next, &ack));
  EXPECT_TRUE(ack);
  EXPECT_EQ(next, 0u);
  EXPECT_EQ(rx.next_offset(), 0u);
  EXPECT_TRUE(st.data.empty() || st.data[0] == 0);
}

TEST(BulkReceiver, ShaMismatchAborts) {
  SinkState st;
  proto::BulkReceiver rx;
  rx.init(make_sink(st));

  std::vector<uint8_t> payload(8, 0xAA);
  uint8_t sha[32] = {0xFF};  // 違う値
  proto::BulkStart start{1, "asset", 8, {0}, 8};
  std::memcpy(start.sha256, sha, 32);
  ASSERT_TRUE(rx.start(start, nullptr));

  uint32_t next;
  bool ack;
  auto c = make_chunk(1, 0, payload.data(), 8);
  ASSERT_TRUE(rx.feed_chunk(c.data(), c.size(), &next, &ack));
  EXPECT_FALSE(rx.finish(1));
  EXPECT_EQ(st.commit_calls, 0);
  EXPECT_EQ(st.abort_calls, 1);
  EXPECT_FALSE(rx.in_progress());
}

TEST(BulkReceiver, ShortTransferAbortsOnSizeMismatch) {
  SinkState st;
  proto::BulkReceiver rx;
  rx.init(make_sink(st));
  uint8_t sha[32] = {0};
  proto::BulkStart start{2, "asset", 16, {0}, 8};  // 宣言 16B
  std::memcpy(start.sha256, sha, 32);
  ASSERT_TRUE(rx.start(start, nullptr));

  uint32_t next;
  bool ack;
  uint8_t d[8] = {0};
  auto c = make_chunk(2, 0, d, 8);  // 8B しか来ない
  ASSERT_TRUE(rx.feed_chunk(c.data(), c.size(), &next, &ack));
  EXPECT_FALSE(rx.finish(2));  // received != size
  EXPECT_FALSE(rx.in_progress());
}

TEST(BulkReceiver, NewIdAbortsOld) {
  SinkState st;
  proto::BulkReceiver rx;
  rx.init(make_sink(st));
  uint8_t sha[32] = {0};
  proto::BulkStart a{1, "asset", 8, {0}, 8};
  std::memcpy(a.sha256, sha, 32);
  ASSERT_TRUE(rx.start(a, nullptr));
  ASSERT_TRUE(rx.in_progress());

  proto::BulkStart b{2, "theme", 8, {0}, 8};
  std::memcpy(b.sha256, sha, 32);
  ASSERT_TRUE(rx.start(b, nullptr));
  EXPECT_EQ(st.abort_calls, 1);   // 旧 id=1 が abort された
  EXPECT_EQ(rx.current_id(), 2);
}

TEST(BulkReceiver, WriteFailureKeepsOffset) {
  SinkState st;
  proto::BulkReceiver rx;
  rx.init(make_sink(st));
  uint8_t sha[32] = {0};
  proto::BulkStart start{4, "asset", 16, {0}, 8};
  std::memcpy(start.sha256, sha, 32);
  ASSERT_TRUE(rx.start(start, nullptr));

  st.write_ok = false;
  uint8_t d[8] = {0};
  uint32_t next = 0;
  bool ack = false;
  auto c = make_chunk(4, 0, d, 8);
  ASSERT_TRUE(rx.feed_chunk(c.data(), c.size(), &next, &ack));
  EXPECT_TRUE(ack);              // 書き込み失敗 → ACK で現在位置を知らせる
  EXPECT_EQ(next, 0u);
  EXPECT_EQ(rx.next_offset(), 0u);
  EXPECT_TRUE(rx.in_progress()); // 転送はまだ生きている (再送可能)

  // 復帰して同じチャンクを再送したら受理される
  st.write_ok = true;
  ASSERT_TRUE(rx.feed_chunk(c.data(), c.size(), &next, &ack));
  EXPECT_EQ(rx.next_offset(), 8u);
}

TEST(BulkReceiver, BeginReject) {
  SinkState st;
  st.begin_ok = false;
  proto::BulkReceiver rx;
  rx.init(make_sink(st));
  proto::BulkStart start{1, "asset", 8, {0}, 8};
  EXPECT_FALSE(rx.start(start, nullptr));
  EXPECT_FALSE(rx.in_progress());
}
