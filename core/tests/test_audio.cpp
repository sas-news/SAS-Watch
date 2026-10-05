// 音声メモ関連のホストテスト。
//   - IMA-ADPCM / ADP1 コンテナの往復
//   - BULK 送信側ヘルパ (START/END/ACK parse/CHUNK head)
//   - Memo の音声拡張 (録音トグル・秒数上限・追い出し・再生・削除)
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "fakes.hpp"
#include "watch/adpcm.hpp"
#include "watch/features/memo.hpp"
#include "watch/power.hpp"
#include "watch/protocol/bulk.hpp"
#include "watch/protocol/sha256.hpp"
#include "watch/settings.hpp"

using namespace watch;

// ---------- ADP1 / IMA-ADPCM ----------

TEST(Adp1, HeaderRoundTrip) {
  uint8_t h[audio::kAdp1HeaderSize];
  audio::adp1_write_header(h, 12345);
  uint32_t samples = 0;
  const uint8_t* body = nullptr;
  size_t body_len = 0;
  std::vector<uint8_t> file(h, h + sizeof(h));
  file.push_back(0xAA);
  file.push_back(0xBB);
  ASSERT_TRUE(audio::adp1_parse(file.data(), file.size(), &samples, &body,
                                &body_len));
  EXPECT_EQ(samples, 12345u);
  ASSERT_EQ(body_len, 2u);
  EXPECT_EQ(body[0], 0xAA);
}

TEST(Adp1, ParseRejectsJunk) {
  uint8_t bad[20] = "not-an-adp1-file-xx";
  uint32_t s;
  const uint8_t* b;
  size_t bl;
  EXPECT_FALSE(audio::adp1_parse(bad, sizeof(bad), &s, &b, &bl));
  uint8_t shorty[4] = {'A', 'D', 'P', '1'};
  EXPECT_FALSE(audio::adp1_parse(shorty, sizeof(shorty), &s, &b, &bl));
}

TEST(Adpcm, EncodeDecodeRoundTrip) {
  // 1kHz 正弦波 1 秒分を往復させる。IMA-ADPCM は可逆ではないので
  // 形状だけ確認する (相関で 0.95 以上)。
  constexpr size_t kN = audio::kAdpcmSampleRate;
  std::vector<int16_t> pcm(kN);
  for (size_t i = 0; i < kN; ++i) {
    pcm[i] = static_cast<int16_t>(
        12000 * std::sin(2.0 * M_PI * 1000 * i / audio::kAdpcmSampleRate));
  }
  std::vector<uint8_t> enc(kN / 2 + 8);
  audio::ImaAdpcmEncoder e;
  e.reset();
  const size_t enc_n = e.encode(pcm.data(), kN, enc.data(), enc.size());
  const size_t tail = e.flush(enc.data() + enc_n, enc.size() - enc_n);
  EXPECT_EQ(enc_n + tail, kN / 2);  // 4bit/sample

  std::vector<int16_t> dec(kN + 8);
  audio::ImaAdpcmDecoder d;
  d.reset();
  const size_t dec_n = d.decode(enc.data(), enc_n + tail, dec.data(), dec.size());
  EXPECT_EQ(dec_n, kN);

  double dot = 0, a = 0, b = 0;
  for (size_t i = 64; i < kN; ++i) {  // 先頭の立ち上がりを除く
    dot += static_cast<double>(pcm[i]) * dec[i];
    a += static_cast<double>(pcm[i]) * pcm[i];
    b += static_cast<double>(dec[i]) * dec[i];
  }
  EXPECT_GT(dot / std::sqrt(a * b), 0.95);
}

TEST(Adpcm, SplitEncodeSameAsSingle) {
  int16_t pcm[1000];
  for (int i = 0; i < 1000; ++i) pcm[i] = static_cast<int16_t>((i * 37) % 30000 - 15000);
  audio::ImaAdpcmEncoder e1;
  e1.reset();
  uint8_t one[510];
  const size_t n1 = e1.encode(pcm, 1000, one, sizeof(one)) +
                    e1.flush(one + 0, sizeof(one));
  audio::ImaAdpcmEncoder e2;
  e2.reset();
  uint8_t two[510];
  size_t n2 = 0;
  for (size_t off = 0; off < 1000; off += 333) {
    const size_t part = (off + 333 > 1000) ? 1000 - off : 333;
    n2 += e2.encode(pcm + off, part, two + n2, sizeof(two) - n2);
  }
  n2 += e2.flush(two + n2, sizeof(two) - n2);
  ASSERT_EQ(n1, n2);
  EXPECT_EQ(std::memcmp(one, two, n1), 0);
}

// ---------- BULK 送信側ヘルパ ----------

TEST(BulkSender, StartRoundTrip) {
  uint8_t sha[32];
  for (int i = 0; i < 32; ++i) sha[i] = static_cast<uint8_t>(i);
  uint8_t buf[128];
  const size_t n =
      proto::bulk_encode_start(7, "memo", 65536, sha, 1024, buf, sizeof(buf));
  ASSERT_GT(n, 0u);
  proto::BulkStart bs;
  ASSERT_TRUE(proto::bulk_parse_start(buf, n, &bs));
  EXPECT_EQ(bs.id, 7);
  EXPECT_STREQ(bs.kind, "memo");
  EXPECT_EQ(bs.size, 65536u);
  EXPECT_EQ(bs.chunk, 1024u);
  EXPECT_EQ(std::memcmp(bs.sha256, sha, 32), 0);
}

TEST(BulkSender, EndAndAckRoundTrip) {
  uint8_t buf[64];
  const size_t n = proto::bulk_encode_end(42, buf, sizeof(buf));
  ASSERT_GT(n, 0u);
  uint16_t id = 0;
  ASSERT_TRUE(proto::bulk_parse_end(buf, n, &id));
  EXPECT_EQ(id, 42);

  const size_t an = proto::bulk_encode_ack(42, 8192, buf, sizeof(buf));
  ASSERT_GT(an, 0u);
  uint16_t aid = 0;
  uint32_t next = 0;
  ASSERT_TRUE(proto::bulk_parse_ack(buf, an, &aid, &next));
  EXPECT_EQ(aid, 42);
  EXPECT_EQ(next, 8192u);
}

TEST(BulkSender, ChunkHeadIsLittleEndian) {
  uint8_t head[proto::kBulkChunkHead];
  proto::bulk_chunk_head(0x1234, 0xDEADBEEF, head);
  EXPECT_EQ(head[0], 0x34);
  EXPECT_EQ(head[1], 0x12);
  EXPECT_EQ(head[2], 0xEF);
  EXPECT_EQ(head[3], 0xBE);
  EXPECT_EQ(head[4], 0xAD);
  EXPECT_EQ(head[5], 0xDE);
}

// ---------- Memo 音声拡張 ----------

namespace {

struct AudioFixture {
  EventBus bus;
  test::MemoryKeyValueStore kv;
  test::FakeClock clock;
  PowerPolicy power{&bus};
  test::FakeAudio audio;
  test::EventRecorder rec;
  Settings settings;
  FeatureContext ctx;

  AudioFixture() : ctx{bus, kv, clock, nullptr, &power} {
    audio.set_clock(&clock);
    ctx.audio = &audio;
    ctx.settings = &settings;
    rec.attach_all(bus);
    features::memo_reset_state();
  }

  void act(ActionType t, uint32_t arg0 = 0) {
    Action a;
    a.type = t;
    a.arg0 = arg0;
    features::kMemo.handle(a, ctx);
  }
};

}  // namespace

TEST(MemoVoice, RecordToggleSavesEntry) {
  AudioFixture f;
  f.act(ActionType::MemoRecordStart);
  ASSERT_TRUE(features::memo_recording());
  const uint32_t id = features::memo_record_id();
  EXPECT_GT(id, 0u);
  EXPECT_EQ(f.power.lease_count(), 1u);

  f.clock.advance_ms(5000);  // 5 秒録音した体
  f.act(ActionType::MemoRecordStart);  // トグルで停止→確定
  EXPECT_FALSE(features::memo_recording());
  EXPECT_EQ(f.power.lease_count(), 0u);

  ASSERT_EQ(features::memo_count(), 1u);
  features::MemoEntry e;
  ASSERT_TRUE(features::memo_at(0, &e));
  EXPECT_EQ(e.id, id);
  EXPECT_EQ(e.kind, features::MemoKind::Voice);
  EXPECT_EQ(e.sec, 5u);
  EXPECT_GT(e.size, 0u);
  const auto* ev = f.rec.last_of(EventType::MemoSaved);
  ASSERT_NE(ev, nullptr);
  EXPECT_EQ(ev->arg0, id);
}

TEST(MemoVoice, TickAutoStopsAtMaxSec) {
  AudioFixture f;
  f.act(ActionType::MemoRecordStart);
  ASSERT_TRUE(features::memo_recording());
  f.audio.rec_elapsed_s = features::kMemoVoiceMaxSec;
  features::kMemo.tick(0, f.ctx);
  EXPECT_FALSE(features::memo_recording());
  features::MemoEntry e;
  ASSERT_TRUE(features::memo_at(0, &e));
  EXPECT_EQ(e.sec, features::kMemoVoiceMaxSec);
}

TEST(MemoVoice, NoAudioPortFails) {
  AudioFixture f;
  f.ctx.audio = nullptr;
  f.act(ActionType::MemoRecordStart);
  EXPECT_FALSE(features::memo_recording());
}

TEST(MemoVoice, RecordBeginFailureDoesNotBurnIdOnRetry) {
  AudioFixture f;
  f.audio.fail_record_begin = true;
  f.act(ActionType::MemoRecordStart);
  EXPECT_FALSE(features::memo_recording());
  f.audio.fail_record_begin = false;
  f.act(ActionType::MemoRecordStart);
  ASSERT_TRUE(features::memo_recording());
  // 失敗で id を消費したので、成功側は次の id。
  EXPECT_EQ(features::memo_record_id(), 2u);
}

TEST(MemoVoice, VoiceEvictsOldestOverLimit) {
  AudioFixture f;
  // 9 件録音 (上限 8 件)。1 秒ずつで容量は余裕。
  for (int i = 0; i < 9; ++i) {
    f.act(ActionType::MemoRecordStart);
    ASSERT_TRUE(features::memo_recording());
    f.audio.rec_elapsed_s = 1;
    f.act(ActionType::MemoRecordStart);
    ASSERT_FALSE(features::memo_recording());
  }
  size_t voices = 0;
  features::MemoEntry e;
  for (size_t i = 0; i < features::memo_count(); ++i) {
    ASSERT_TRUE(features::memo_at(i, &e));
    if (e.kind == features::MemoKind::Voice) ++voices;
  }
  EXPECT_EQ(voices, features::kMemoMaxVoice);
  ASSERT_TRUE(features::memo_at(0, &e));
  EXPECT_NE(e.id, 1u);  // 最古の 1 は追い出された
  EXPECT_GE(f.audio.erase_calls, 1);
}

TEST(MemoVoice, VoiceBudgetEvicts) {
  AudioFixture f;
  // 容量上限 (1MB) を 60 秒×2 で確実に超える。
  for (int i = 0; i < 3; ++i) {
    f.act(ActionType::MemoRecordStart);
    f.audio.rec_elapsed_s = features::kMemoVoiceMaxSec;  // 60s ≈ 480KB
    f.act(ActionType::MemoRecordStart);
  }
  size_t voices = 0;
  for (size_t i = 0; i < features::memo_count(); ++i) {
    features::MemoEntry e;
    ASSERT_TRUE(features::memo_at(i, &e));
    if (e.kind == features::MemoKind::Voice) ++voices;
  }
  EXPECT_LT(voices, 3u);
}

TEST(MemoVoice, PlayAndStop) {
  AudioFixture f;
  f.act(ActionType::MemoRecordStart);
  f.audio.rec_elapsed_s = 3;
  f.act(ActionType::MemoRecordStart);
  const auto* ev = f.rec.last_of(EventType::MemoSaved);
  ASSERT_NE(ev, nullptr);
  const uint32_t mid = ev->arg0;

  ASSERT_TRUE(features::memo_play(mid, f.ctx));
  EXPECT_EQ(features::memo_playing_id(), mid);
  EXPECT_EQ(f.audio.last_play_vol, 70u);  // settings.audio_volume 既定
  features::memo_stop_play(f.ctx);
  EXPECT_EQ(features::memo_playing_id(), 0u);
}

TEST(MemoVoice, PlayVolumeFromSettings) {
  AudioFixture f;
  f.settings.audio_volume = 30;
  f.audio.put_audio(77, 5);
  // エントリに登録するため一度通す (再生対象はエントリ必須)。
  f.act(ActionType::MemoRecordStart);
  f.audio.rec_elapsed_s = 5;
  f.act(ActionType::MemoRecordStart);
  const auto* ev = f.rec.last_of(EventType::MemoSaved);
  ASSERT_NE(ev, nullptr);
  EXPECT_TRUE(features::memo_play(ev->arg0, f.ctx));
  EXPECT_EQ(f.audio.last_play_vol, 30u);
}

TEST(MemoVoice, DeleteVoiceErasesFileAndPublishes) {
  AudioFixture f;
  f.act(ActionType::MemoRecordStart);
  f.audio.rec_elapsed_s = 2;
  f.act(ActionType::MemoRecordStart);
  const auto* ev = f.rec.last_of(EventType::MemoSaved);
  ASSERT_NE(ev, nullptr);
  const uint32_t id = ev->arg0;

  f.act(ActionType::MemoDelete, id);
  EXPECT_EQ(features::memo_count(), 0u);
  EXPECT_GE(f.audio.erase_calls, 1);
  uint32_t sz;
  EXPECT_FALSE(f.audio.memo_audio_size(id, &sz));
  ASSERT_NE(f.rec.last_of(EventType::MemoDeleted), nullptr);
  EXPECT_EQ(f.rec.last_of(EventType::MemoDeleted)->arg0, id);
}

TEST(MemoVoice, RestoreV1MigratesToText) {
  // v1 (テキストのみ) のデータを手で作って restore する。
  AudioFixture f;
  struct HdrV1 {
    uint8_t version, reserved;
    uint16_t count, head, reserved2;
    uint32_t next_id;
  } h{};
  h.version = 1;
  h.count = 1;
  h.head = 0;
  h.next_id = 2;
  f.kv.set("feat.memo.hdr", &h, sizeof(h));
  struct SlotV1 {
    uint32_t id;
    uint16_t len;
    char text[256];
  } s{};
  s.id = 1;
  s.len = 4;
  std::memcpy(s.text, "hoge", 4);
  f.kv.set("feat.memo.00", &s, sizeof(s));

  features::kMemo.restore(f.ctx);
  ASSERT_EQ(features::memo_count(), 1u);
  features::MemoEntry e;
  ASSERT_TRUE(features::memo_at(0, &e));
  EXPECT_EQ(e.kind, features::MemoKind::Text);
  EXPECT_STREQ(e.text, "hoge");
}
