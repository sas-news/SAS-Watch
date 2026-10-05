// Agent Feature。「話しかける」→ 録音 (最大30秒, ADP1) → BULK kind="agent" で
// スマホへ送り、agent.reply REQ の返答を表示する。
// 定型質問 (settings agent.q1..3) は EVT agent.request でテキストを送る。
// 実際の BULK/EVT 送信は ble_glue (firmware) が app タスクのループで行い、
// core は「何を送るか」と状態機械だけを持つ。
// 録音中は Res::Audio の Lease を取って Deep Sleep しない。
#include "watch/features/agent.hpp"

#include <cstdio>
#include <cstring>

#include "watch/power.hpp"
#include "watch/settings.hpp"

namespace watch {
namespace features {

namespace {

AgentPhase s_phase = AgentPhase::Idle;
uint16_t s_req_id = 0;                    // 現在の要求 id (0 = なし)
uint16_t s_next_id = 1;
int64_t s_deadline_ms = 0;                // Sending/Thinking の打ち切り時刻

// 送信待ち (ble_glue が拾う)。Sending に入っても送信が始まるまでは true。
bool s_pending_audio = false;
uint32_t s_pending_file = 0;
bool s_pending_has_text = false;
char s_pending_text[kAgentQuestionMax + 1] = {};

char s_reply[kAgentReplyMax + 1] = {};
char s_error[96] = {};
PowerPolicy::Lease s_lease;

uint16_t next_id() {
  const uint16_t id = s_next_id++;
  if (s_next_id == 0) s_next_id = 1;
  return id;
}

void set_phase(FeatureContext& ctx, AgentPhase p) {
  if (s_phase == p) return;
  s_phase = p;
  ctx.bus.publish(
      {EventType::AgentStatusChanged, static_cast<uint32_t>(p)});
}

void set_error(FeatureContext& ctx, const char* msg) {
  std::snprintf(s_error, sizeof(s_error), "%s", msg ? msg : "エラー");
  set_phase(ctx, AgentPhase::Error);
}

// 録音を終わらせて送信キューへ。commit 済みの音声は kAgentAudioFileId。
void finish_record(FeatureContext& ctx, bool commit) {
  uint32_t sec = 0, size = 0;
  const bool ok = ctx.audio && ctx.audio->record_end(commit, &sec, &size);
  s_lease.release();
  if (!ok || !commit || size == 0 || sec == 0) {
    if (ctx.audio) ctx.audio->memo_audio_erase(kAgentAudioFileId);
    set_phase(ctx, AgentPhase::Idle);
    return;
  }
  s_req_id = next_id();
  s_pending_audio = true;
  s_pending_file = kAgentAudioFileId;
  s_deadline_ms = ctx.clock.now_ms() + kAgentTimeoutMs;
  set_phase(ctx, AgentPhase::Sending);
}

void clear_request() {
  s_req_id = 0;
  s_deadline_ms = 0;
  s_pending_audio = false;
  s_pending_file = 0;
  s_pending_has_text = false;
  s_pending_text[0] = '\0';
}

bool handle(const Action& a, FeatureContext& ctx) {
  switch (a.type) {
    case ActionType::PrimaryAction:  // 画面の主ボタン = 話しかける
    case ActionType::AgentRecordToggle:
      if (s_phase == AgentPhase::Recording) {
        finish_record(ctx, true);  // 停止して送信
        return true;
      }
      if (s_phase != AgentPhase::Idle && s_phase != AgentPhase::Reply &&
          s_phase != AgentPhase::Error) {
        return true;  // Sending/Thinking 中は無視
      }
      if (!ctx.audio || !ctx.audio->record_begin(kAgentAudioFileId,
                                                 kAgentMaxRecordSec)) {
        set_error(ctx, "録音を開始できませんでした");
        return true;
      }
      clear_request();
      s_reply[0] = '\0';
      if (ctx.power) s_lease = ctx.power->acquire(Res::Audio, "agent");
      set_phase(ctx, AgentPhase::Recording);
      return true;

    case ActionType::AgentAsk: {
      if (s_phase != AgentPhase::Idle && s_phase != AgentPhase::Reply &&
          s_phase != AgentPhase::Error) {
        return true;  // 処理中は無視
      }
      const char* q = agent_question(a.arg0, ctx.settings);
      if (!q || !*q) return true;
      clear_request();
      s_reply[0] = '\0';
      s_req_id = next_id();
      s_pending_has_text = true;
      std::snprintf(s_pending_text, sizeof(s_pending_text), "%s", q);
      s_deadline_ms = ctx.clock.now_ms() + kAgentTimeoutMs;
      set_phase(ctx, AgentPhase::Sending);
      return true;
    }

    case ActionType::AgentClear:
      // 表示中の返答/エラーを閉じる (処理中のキャンセルには使わない)。
      if (s_phase == AgentPhase::Reply || s_phase == AgentPhase::Error) {
        s_reply[0] = '\0';
        s_error[0] = '\0';
        set_phase(ctx, AgentPhase::Idle);
      }
      return true;

    default:
      return false;
  }
}

void tick(int64_t now_ms, FeatureContext& ctx) {
  // 上限/外部停止に追従して録音を確定する。
  if (s_phase == AgentPhase::Recording && ctx.audio) {
    if (!ctx.audio->recording() ||
        ctx.audio->record_elapsed_s() >= kAgentMaxRecordSec) {
      finish_record(ctx, true);
    }
  }
  // 送信/返答待ちの間に BLE が切れたらエラー (返答は届かない)。
  if ((s_phase == AgentPhase::Sending || s_phase == AgentPhase::Thinking) &&
      ctx.power && !ctx.power->ble_connected()) {
    if (ctx.audio) ctx.audio->memo_audio_erase(kAgentAudioFileId);
    set_error(ctx, "スマホとつながっていません");
    return;
  }
  // 全体のタイムアウト (送信開始から60秒)。
  if ((s_phase == AgentPhase::Sending || s_phase == AgentPhase::Thinking) &&
      s_deadline_ms != 0 && now_ms >= s_deadline_ms) {
    if (ctx.audio) ctx.audio->memo_audio_erase(kAgentAudioFileId);
    set_error(ctx, "応答がありませんでした");
  }
}

int64_t next_deadline_ms() {
  if (s_phase == AgentPhase::Sending || s_phase == AgentPhase::Thinking) {
    return s_deadline_ms;
  }
  return 0;
}

}  // namespace

AgentPhase agent_phase() { return s_phase; }
const char* agent_reply_text() { return s_reply; }
const char* agent_error_text() { return s_error; }
bool agent_recording() { return s_phase == AgentPhase::Recording; }

uint32_t agent_record_elapsed_s(FeatureContext& ctx) {
  return (s_phase == AgentPhase::Recording && ctx.audio)
             ? ctx.audio->record_elapsed_s()
             : 0;
}

uint8_t agent_record_level(FeatureContext& ctx) {
  return (s_phase == AgentPhase::Recording && ctx.audio)
             ? ctx.audio->record_level()
             : 0;
}

const char* agent_question(size_t i, const Settings* s) {
  if (!s || i >= kAgentQuestionCount) return nullptr;
  const char* q = i == 0 ? s->agent_q1 : i == 1 ? s->agent_q2 : s->agent_q3;
  return (q && *q) ? q : nullptr;
}

bool agent_pending(AgentPending* out) {
  if (!out || s_phase != AgentPhase::Sending) return false;
  if (s_pending_audio) {
    out->audio = true;
    out->id = s_req_id;
    out->file_id = s_pending_file;
    return true;
  }
  if (s_pending_has_text) {
    out->audio = false;
    out->id = s_req_id;
    out->file_id = 0;
    std::memcpy(out->text, s_pending_text, sizeof(s_pending_text));
    return true;
  }
  return false;
}

void agent_send_started(uint16_t id) {
  if (s_phase != AgentPhase::Sending || id != s_req_id) return;
  s_pending_audio = false;
  s_pending_has_text = false;
}

void agent_sent(uint16_t id, bool ok, FeatureContext& ctx) {
  // BULK (音声) が全部出た/失敗した、または EVT (テキスト) を出した。
  // 音声ファイルはここで消す (メモ領域を戻す)。
  if (s_phase != AgentPhase::Sending || id != s_req_id) return;
  s_pending_audio = false;
  s_pending_has_text = false;
  if (ctx.audio) ctx.audio->memo_audio_erase(kAgentAudioFileId);
  if (ok) {
    set_phase(ctx, AgentPhase::Thinking);
  } else {
    set_error(ctx, "送信に失敗しました");
  }
}

bool agent_on_reply(uint16_t id, const char* text, size_t len,
                    FeatureContext& ctx) {
  if (!text || s_phase != AgentPhase::Thinking || id != s_req_id) {
    return false;
  }
  size_t n = len;
  if (n > kAgentReplyMax) n = kAgentReplyMax;
  // UTF-8 の途中で切らない: 末尾の文字が完全に入っていなければ捨てる。
  // 最後の文字の先頭バイトまで戻り、必要バイト数が収まっているか確かめる。
  size_t head = n;
  while (head > 0 && (static_cast<uint8_t>(text[head - 1]) & 0xC0) == 0x80) {
    --head;
  }
  if (head > 0) {
    const uint8_t lead = static_cast<uint8_t>(text[head - 1]);
    const size_t need = lead < 0x80   ? 1
                        : lead < 0xE0 ? 2
                        : lead < 0xF0 ? 3
                        : lead < 0xF8 ? 4
                                      : 1;
    if (n - (head - 1) < need) n = head - 1;
  }
  std::memcpy(s_reply, text, n);
  s_reply[n] = '\0';
  if (ctx.power) ctx.power->kick_activity(ctx.clock.now_ms());
  set_phase(ctx, AgentPhase::Reply);
  return true;
}

void agent_reset_state() {
  s_phase = AgentPhase::Idle;
  clear_request();
  s_next_id = 1;
  s_reply[0] = '\0';
  s_error[0] = '\0';
  s_lease.release();
}

const FeatureDescriptor kAgent = {
    /*id*/ "agent",
    /*capabilities*/ HasScreen | NeedsAudio,
    /*route*/ Route::Agent,
    /*init*/ nullptr,
    /*handle*/ handle,
    /*tick*/ tick,
    /*next_deadline_ms*/ next_deadline_ms,
    /*save*/ nullptr,
    /*restore*/ nullptr,
};

}  // namespace features
}  // namespace watch
