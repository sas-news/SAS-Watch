// Feature (Timer/Stopwatch/Counter/Memo/Agent) と Settings のホストテスト。
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "watch/features/agent.hpp"
#include "watch/features/counter.hpp"
#include "watch/features/memo.hpp"
#include "watch/features/stopwatch.hpp"
#include "watch/features/timer.hpp"
#include "watch/power.hpp"
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
    features::agent_reset_state();
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

TEST(Timer, PauseKeepsRemaining) {
  FeatFixture f;
  f.clock.set_now_ms(0);
  f.act(features::kTimer, ActionType::TimerStart, 90);
  f.clock.advance_ms(30'000);
  f.act(features::kTimer, ActionType::TimerPause);
  const auto& st = features::timer_state();
  EXPECT_FALSE(st.running);
  EXPECT_TRUE(st.paused);
  EXPECT_EQ(st.paused_ms, 60'000);
  EXPECT_EQ(features::timer_remaining_ms(f.clock.now_ms()), 60'000);
  EXPECT_EQ(f.rec.last_of(EventType::TimerPaused)->arg0, 60u);
  // 一時停止中に時間が進んでも残りは減らないし、終了イベントも出ない。
  f.clock.advance_ms(120'000);
  features::kTimer.tick(f.clock.now_ms(), f.ctx);
  EXPECT_FALSE(features::timer_state().finished);
  EXPECT_EQ(features::timer_remaining_ms(f.clock.now_ms()), 60'000);
}

TEST(Timer, ResumeFromPause) {
  FeatFixture f;
  f.clock.set_now_ms(0);
  f.act(features::kTimer, ActionType::TimerStart, 90);
  f.clock.advance_ms(30'000);
  f.act(features::kTimer, ActionType::TimerPause);
  f.clock.advance_ms(5'000);
  f.act(features::kTimer, ActionType::TimerStart, 0);  // arg0=0 で再開
  EXPECT_TRUE(features::timer_state().running);
  EXPECT_FALSE(features::timer_state().paused);
  // 残り 60s が今から数え直しになる。
  EXPECT_EQ(features::timer_remaining_ms(f.clock.now_ms()), 60'000);
}

TEST(Timer, AddMinuteWhileRunning) {
  FeatFixture f;
  f.clock.set_now_ms(0);
  f.act(features::kTimer, ActionType::TimerStart, 60);
  f.clock.advance_ms(10'000);
  f.act(features::kTimer, ActionType::TimerAddMinute);
  EXPECT_TRUE(features::timer_state().running);
  EXPECT_EQ(features::timer_remaining_ms(f.clock.now_ms()), 110'000);
  EXPECT_EQ(features::timer_state().duration_s, 120u);
}

TEST(Timer, AddMinuteAfterFinishStarts60s) {
  // 「もう1分」: 終了アラートから押すと 60 秒タイマーとして再開。
  FeatFixture f;
  f.clock.set_now_ms(0);
  f.act(features::kTimer, ActionType::TimerStart, 10);
  f.clock.advance_ms(10'000);
  features::kTimer.tick(f.clock.now_ms(), f.ctx);
  EXPECT_TRUE(features::timer_state().finished);
  f.act(features::kTimer, ActionType::TimerAddMinute);
  const auto& st = features::timer_state();
  EXPECT_TRUE(st.running);
  EXPECT_FALSE(st.finished);
  EXPECT_EQ(st.duration_s, 60u);
  EXPECT_EQ(features::timer_remaining_ms(f.clock.now_ms()), 60'000);
}

TEST(Timer, AddMinuteWhilePausedExtends) {
  FeatFixture f;
  f.clock.set_now_ms(0);
  f.act(features::kTimer, ActionType::TimerStart, 60);
  f.act(features::kTimer, ActionType::TimerPause);
  f.act(features::kTimer, ActionType::TimerAddMinute);
  EXPECT_TRUE(features::timer_state().paused);
  EXPECT_EQ(features::timer_remaining_ms(f.clock.now_ms()), 120'000);
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

// ---------- Agent (AI) ----------

// audio/power/settings が必要なので配線済みの fixture を足す。
struct AgentFixture : FeatFixture {
  test::FakeAudio audio;
  PowerPolicy power{&bus};
  Settings settings;

  AgentFixture() {
    audio.set_clock(&clock);
    ctx.power = &power;
    ctx.audio = &audio;
    ctx.settings = &settings;
    power.set_ble_connected(true);  // BLE ありが既定 (切るテストは false に)
  }

  void tick(int64_t at) { features::kAgent.tick(at, ctx); }
};

TEST(Agent, RecordStopQueuesAudioSend) {
  AgentFixture f;
  f.clock.set_now_ms(10'000);
  f.act(features::kAgent, ActionType::AgentRecordToggle);
  EXPECT_EQ(features::agent_phase(), features::AgentPhase::Recording);
  EXPECT_TRUE(features::agent_recording());
  EXPECT_GT(f.power.lease_count(), 0);  // 録音中は Deep Sleep しない
  EXPECT_EQ(f.audio.record_begin_calls, 1);

  f.clock.advance_ms(5'000);
  f.act(features::kAgent, ActionType::AgentRecordToggle);  // 停止→送信へ
  EXPECT_EQ(features::agent_phase(), features::AgentPhase::Sending);
  // 録音ファイルは確定済み (5秒, kAgentAudioFileId)。
  uint32_t size = 0;
  EXPECT_TRUE(f.audio.memo_audio_size(features::kAgentAudioFileId, &size));
  EXPECT_EQ(size, test::FakeAudio::kFileHeader +
                      5 * test::FakeAudio::kBytesPerSec);

  features::AgentPending p;
  ASSERT_TRUE(features::agent_pending(&p));
  EXPECT_TRUE(p.audio);
  EXPECT_EQ(p.file_id, features::kAgentAudioFileId);
  EXPECT_NE(p.id, 0);
  // 送信開始→ pending 閉鎖。BULK 完了で Thinking。
  features::agent_send_started(p.id);
  EXPECT_FALSE(features::agent_pending(&p));
  features::agent_sent(p.id, true, f.ctx);
  EXPECT_EQ(features::agent_phase(), features::AgentPhase::Thinking);
  // 音声ファイルは送信完了で消える。
  EXPECT_FALSE(
      f.audio.memo_audio_size(features::kAgentAudioFileId, nullptr));
}

TEST(Agent, RecordingAutoStopsAt30s) {
  AgentFixture f;
  f.act(features::kAgent, ActionType::AgentRecordToggle);
  f.audio.rec_elapsed_s = features::kAgentMaxRecordSec;
  f.tick(0);
  EXPECT_EQ(features::agent_phase(), features::AgentPhase::Sending);
}

TEST(Agent, RecordBeginFailureShowsError) {
  AgentFixture f;
  f.audio.fail_record_begin = true;
  f.act(features::kAgent, ActionType::AgentRecordToggle);
  EXPECT_EQ(features::agent_phase(), features::AgentPhase::Error);
  EXPECT_STREQ(features::agent_error_text(), "録音を開始できませんでした");
}

TEST(Agent, AskSendsQuestionText) {
  AgentFixture f;
  std::snprintf(f.settings.agent_q1, sizeof(f.settings.agent_q1), "%s",
                "今日の予定は？");
  f.act(features::kAgent, ActionType::AgentAsk, 0);
  EXPECT_EQ(features::agent_phase(), features::AgentPhase::Sending);
  features::AgentPending p;
  ASSERT_TRUE(features::agent_pending(&p));
  EXPECT_FALSE(p.audio);
  EXPECT_STREQ(p.text, "今日の予定は？");
  features::agent_sent(p.id, true, f.ctx);  // EVT を出した
  EXPECT_EQ(features::agent_phase(), features::AgentPhase::Thinking);
}

TEST(Agent, AskEmptyQuestionDoesNothing) {
  AgentFixture f;
  f.settings.agent_q3[0] = '\0';
  f.act(features::kAgent, ActionType::AgentAsk, 2);
  EXPECT_EQ(features::agent_phase(), features::AgentPhase::Idle);
}

TEST(Agent, ReplyShowsText) {
  AgentFixture f;
  f.act(features::kAgent, ActionType::AgentAsk, 0);  // 既定: 今日の予定は？
  features::AgentPending p;
  ASSERT_TRUE(features::agent_pending(&p));
  features::agent_sent(p.id, true, f.ctx);

  const char* reply = "今日は15時に会議があります。";
  EXPECT_TRUE(features::agent_on_reply(p.id, reply, std::strlen(reply),
                                       f.ctx));
  EXPECT_EQ(features::agent_phase(), features::AgentPhase::Reply);
  EXPECT_STREQ(features::agent_reply_text(), reply);
  const auto* ev = f.rec.last_of(EventType::AgentStatusChanged);
  ASSERT_NE(ev, nullptr);
  EXPECT_EQ(ev->arg0,
            static_cast<uint32_t>(features::AgentPhase::Reply));

  // 別 id の返答は無視される。
  const char* late = "遅れた返答";
  EXPECT_FALSE(features::agent_on_reply(p.id + 1, late, std::strlen(late),
                                      f.ctx));
  EXPECT_STREQ(features::agent_reply_text(), reply);

  f.act(features::kAgent, ActionType::AgentClear);
  EXPECT_EQ(features::agent_phase(), features::AgentPhase::Idle);
}

TEST(Agent, ReplyTruncatesLongText) {
  AgentFixture f;
  f.act(features::kAgent, ActionType::AgentAsk, 0);
  features::AgentPending p;
  ASSERT_TRUE(features::agent_pending(&p));
  features::agent_sent(p.id, true, f.ctx);

  std::string long_text(features::kAgentReplyMax + 300, 'x');
  EXPECT_TRUE(features::agent_on_reply(p.id, long_text.data(),
                                       long_text.size(), f.ctx));
  EXPECT_EQ(std::strlen(features::agent_reply_text()),
            features::kAgentReplyMax);
}

TEST(Agent, TimeoutShowsError) {
  AgentFixture f;
  f.clock.set_now_ms(0);
  f.act(features::kAgent, ActionType::AgentAsk, 0);
  features::AgentPending p;
  ASSERT_TRUE(features::agent_pending(&p));
  features::agent_sent(p.id, true, f.ctx);
  EXPECT_EQ(features::agent_phase(), features::AgentPhase::Thinking);
  EXPECT_EQ(features::kAgent.next_deadline_ms(), features::kAgentTimeoutMs);

  f.tick(features::kAgentTimeoutMs - 1);
  EXPECT_EQ(features::agent_phase(), features::AgentPhase::Thinking);
  f.tick(features::kAgentTimeoutMs);
  EXPECT_EQ(features::agent_phase(), features::AgentPhase::Error);
  EXPECT_STREQ(features::agent_error_text(), "応答がありませんでした");
}

TEST(Agent, BleDisconnectShowsError) {
  AgentFixture f;
  f.act(features::kAgent, ActionType::AgentAsk, 0);
  features::AgentPending p;
  ASSERT_TRUE(features::agent_pending(&p));
  features::agent_sent(p.id, true, f.ctx);
  f.power.set_ble_connected(false);
  f.tick(1000);
  EXPECT_EQ(features::agent_phase(), features::AgentPhase::Error);
  EXPECT_STREQ(features::agent_error_text(), "スマホとつながっていません");
}

TEST(Agent, SendFailureShowsError) {
  AgentFixture f;
  f.act(features::kAgent, ActionType::AgentRecordToggle);
  f.clock.advance_ms(5'000);  // 1秒未満の録音は捨てられるので進める
  f.act(features::kAgent, ActionType::AgentRecordToggle);
  features::AgentPending p;
  ASSERT_TRUE(features::agent_pending(&p));
  features::agent_send_started(p.id);
  features::agent_sent(p.id, false, f.ctx);
  EXPECT_EQ(features::agent_phase(), features::AgentPhase::Error);
  EXPECT_STREQ(features::agent_error_text(), "送信に失敗しました");
}

TEST(Agent, BusyIgnoresNewRequest) {
  AgentFixture f;
  f.act(features::kAgent, ActionType::AgentAsk, 0);
  features::AgentPending p1;
  ASSERT_TRUE(features::agent_pending(&p1));
  // 処理中に別の質問を押しても現在の要求は変わらない。
  f.act(features::kAgent, ActionType::AgentAsk, 1);
  features::AgentPending p2;
  ASSERT_TRUE(features::agent_pending(&p2));
  EXPECT_EQ(p2.id, p1.id);
}
