// Agent Feature (T38)。「話しかける」= 音声入力 (文字起こし) であって録音ではない。
// 開始と同時に 16kHz IMA ADPCM を BULK kind="dictation" でスマホへリアルタイム
// 送出し、スマホが返す部分文字起こし (REQ agent.partial) を聞き取り中に表示する。
// 停止で BULK_END を送り、確定テキスト → agent.reply で Muse の返答を表示する。
// **音声は保存しない**: この Feature も audio 実装も音声ファイルを作らず、
// PSRAM のリングバッファ (揮発) を BLE へ流すだけ。停止でリングも解放する。
// 話した内容は部分文字起こしと Muse へのテキストだけが残る。
// 定型質問 (settings agent.q1..3) は従来どおり EVT agent.request で送る。
// 実際の BULK/EVT 送信は ble_glue (firmware) が app タスクのループで行い、
// core は「何を送るか」と状態機械だけを持つ。
// 聞き取り中は Res::Audio の Lease を取って Deep Sleep しない。
#include "watch/features/agent.hpp"

#include <cstdio>
#include <cstring>

#include "watch/power.hpp"
#include "watch/settings.hpp"

namespace watch {
namespace features {

namespace {

AgentPhase s_phase = AgentPhase::Idle;
uint16_t s_req_id = 0;      // 現在の要求 id (0 = なし)。dictation の talk id。
uint16_t s_next_id = 1;
int64_t s_deadline_ms = 0;       // Sending/Thinking の打ち切り時刻
int64_t s_pump_deadline_ms = 0;  // dictation の送出しポンプ起床時刻

// 定型質問 (EVT agent.request) の送信待ち。
bool s_has_pending_text = false;
char s_pending_text[kAgentQuestionMax + 1] = {};

// dictation ストリーム (BULK kind="dictation")。
bool s_stream_mode = false;          // 現在の要求が dictation か
AgentStreamCmd s_stream_cmd = AgentStreamCmd::None;
uint16_t s_stream_id = 0;
bool s_stream_started = false;       // BULK_START を送れた
bool s_partial_final = false;        // agent.partial の final:true を受けた
char s_partial[kAgentPartialMax + 1] = {};
char s_notice[64] = {};              // 聞き取り中の警告 (旧アプリ検出など)
char s_after_stop_error[96] = {};    // BULK_END 後に Error へ落とす理由

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

// src の先頭 len バイトを dst (cap バイト、NUL 含む) にコピーする。
// cap で切る位置が UTF-8 の途中なら、不完全な文字を落とす。
size_t copy_utf8(char* dst, size_t cap, const char* src, size_t len) {
  if (!dst || cap == 0) return 0;
  size_t n = (!src) ? 0 : (len < cap - 1 ? len : cap - 1);
  size_t head = n;
  while (head > 0 && (static_cast<uint8_t>(src[head - 1]) & 0xC0) == 0x80) {
    --head;
  }
  if (head > 0) {
    const uint8_t lead = static_cast<uint8_t>(src[head - 1]);
    const size_t need = lead < 0x80   ? 1
                        : lead < 0xE0 ? 2
                        : lead < 0xF0 ? 3
                        : lead < 0xF8 ? 4
                                      : 1;
    if (n - (head - 1) < need) n = head - 1;
  }
  if (n > 0) std::memcpy(dst, src, n);
  dst[n] = '\0';
  return n;
}

void clear_partial(FeatureContext& ctx) {
  s_partial[0] = '\0';
  s_partial_final = false;
  ctx.bus.publish({EventType::AgentPartialChanged, 0});
}

void clear_request() {
  s_req_id = 0;
  s_deadline_ms = 0;
  s_pump_deadline_ms = 0;
  s_has_pending_text = false;
  s_pending_text[0] = '\0';
  s_stream_mode = false;
  s_stream_cmd = AgentStreamCmd::None;
  s_stream_id = 0;
  s_stream_started = false;
  s_notice[0] = '\0';
  s_after_stop_error[0] = '\0';
}

// 音声入力とストリームを確実に畳む。失敗・切断・エラー・キャンセルの後始末は
// すべてここを通す (リングバッファの解放と lease の返却を漏らさない)。
// 音声は保存しないので、後始末で消すべき音声ファイルは無い (仮 id の erase は
// メモ録音経路との取り違え防止のための無害な no-op)。
void abort_audio(FeatureContext& ctx) {
  if (ctx.audio) {
    if (ctx.audio->recording()) {
      uint32_t sec = 0, size = 0;
      ctx.audio->record_end(false, &sec, &size);
    }
    ctx.audio->record_stream_enable(false);  // リングバッファ解放
    ctx.audio->memo_audio_erase(kAgentAudioFileId);
  }
  s_lease.release();
  s_stream_cmd = AgentStreamCmd::None;
  s_stream_started = false;
  s_notice[0] = '\0';
  s_pump_deadline_ms = 0;
}

void set_error(FeatureContext& ctx, const char* msg) {
  std::snprintf(s_error, sizeof(s_error), "%s", msg ? msg : "エラー");
  abort_audio(ctx);
  s_stream_mode = false;
  s_deadline_ms = 0;
  set_phase(ctx, AgentPhase::Error);
}

// 聞き取りを終わらせて「残りを吐いて BULK_END」の段階 (Sending) へ。
void finish_record(FeatureContext& ctx) {
  uint32_t sec = 0, size = 0;
  const bool ok = ctx.audio && ctx.audio->record_end(false, &sec, &size);
  s_lease.release();
  if (!ok) {
    set_error(ctx, "聞き取りを停止できませんでした");
    return;
  }
  if (!s_stream_started) {
    // BULK_START を送れていない (旧アプリ / 未接続)。END も不要。
    set_error(ctx, s_notice[0] ? s_notice : "スマホとつながっていません");
    return;
  }
  if (sec == 0) {
    // 音声が取れなかった。END で ASR を閉じてからエラーにする。
    std::snprintf(s_after_stop_error, sizeof(s_after_stop_error), "%s",
                  "聞き取りできませんでした");
  } else if (s_notice[0]) {
    // 旧アプリ警告中。聞き取りは継続したが、停止時にエラーにする (仕様)。
    std::snprintf(s_after_stop_error, sizeof(s_after_stop_error), "%s",
                  s_notice);
  }
  s_stream_cmd = AgentStreamCmd::Stop;
  s_stream_id = s_req_id;
  s_pump_deadline_ms = ctx.clock.now_ms() + kAgentPumpMs;
  s_deadline_ms = ctx.clock.now_ms() + kAgentTimeoutMs;
  set_phase(ctx, AgentPhase::Sending);
}

// 音声入力を開始して talk id を採番する。失敗時は Error。
// ここでは音声入力ジョブを開始するだけで、ファイルは作らない (ストリームのみ)。
bool begin_record(FeatureContext& ctx) {
  if (!ctx.audio || !ctx.audio->can_record_stream()) {
    set_error(ctx, "音声ストリーミングを開始できません");
    return false;
  }
  if (!ctx.audio->record_stream_enable(true)) {
    set_error(ctx, "音声ストリーミングを開始できません");
    return false;
  }
  if (!ctx.audio->record_begin(kAgentAudioFileId, kAgentMaxRecordSec)) {
    // 後始末 (stream 無効化 + lease 解放) は set_error → abort_audio が行う。
    set_error(ctx, "聞き取りを開始できませんでした");
    return false;
  }
  if (ctx.power) s_lease = ctx.power->acquire(Res::Audio, "agent");
  s_req_id = next_id();
  s_stream_mode = true;
  s_stream_started = false;
  s_stream_cmd = AgentStreamCmd::Start;
  s_stream_id = s_req_id;
  s_pump_deadline_ms = ctx.clock.now_ms() + kAgentPumpMs;
  set_phase(ctx, AgentPhase::Recording);
  return true;
}

bool handle(const Action& a, FeatureContext& ctx) {
  switch (a.type) {
    case ActionType::PrimaryAction:  // 画面の主ボタン = 話しかける
    case ActionType::AgentRecordToggle:
      if (s_phase == AgentPhase::Recording) {
        finish_record(ctx);  // 停止 → 残りを吐いて END → Thinking
        return true;
      }
      if (s_phase != AgentPhase::Idle && s_phase != AgentPhase::Reply &&
          s_phase != AgentPhase::Error) {
        return true;  // Sending/Thinking 中は無視
      }
      if (ctx.power && !ctx.power->ble_connected()) {
        set_error(ctx, "スマホとつながっていません");
        return true;
      }
      clear_request();
      s_reply[0] = '\0';
      s_error[0] = '\0';
      clear_partial(ctx);
      begin_record(ctx);
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
      s_error[0] = '\0';
      clear_partial(ctx);
      s_req_id = next_id();
      s_has_pending_text = true;
      std::snprintf(s_pending_text, sizeof(s_pending_text), "%s", q);
      s_deadline_ms = ctx.clock.now_ms() + kAgentTimeoutMs;
      set_phase(ctx, AgentPhase::Sending);
      return true;
    }

    case ActionType::AgentClear:
      // 表示中の返答/エラーを閉じる (処理中のキャンセルには使わない)。
      if (s_phase == AgentPhase::Reply || s_phase == AgentPhase::Error) {
        clear_request();
        s_reply[0] = '\0';
        s_error[0] = '\0';
        s_partial[0] = '\0';
        s_partial_final = false;
        set_phase(ctx, AgentPhase::Idle);
      }
      return true;

    default:
      return false;
  }
}

void tick(int64_t now_ms, FeatureContext& ctx) {
  // 聞き取り中/停止処理中に切断したらエラー (返答は届かない)。
  if ((s_phase == AgentPhase::Recording || s_phase == AgentPhase::Sending) &&
      ctx.power && !ctx.power->ble_connected()) {
    set_error(ctx, "スマホとつながっていません");
    return;
  }
  if (s_phase == AgentPhase::Recording) {
    // 上限 (止め忘れ防止の 3 分) /外部停止 (録音デバイス側の失敗) に追従する。
    if (ctx.audio &&
        (!ctx.audio->recording() ||
         ctx.audio->record_elapsed_s() >= kAgentMaxRecordSec)) {
      finish_record(ctx);
      return;
    }
    // dictation のポンプ周期で app タスクを起こす。
    s_pump_deadline_ms = now_ms + kAgentPumpMs;
    return;
  }
  if (s_phase == AgentPhase::Sending) {
    // 定型質問の EVT も次のポーリングで出す必要がある (Sending のまま
    // タイムアウトまで app タスクが眠らないように短周期で起こす)。
    s_pump_deadline_ms = now_ms + kAgentPumpMs;
    if (s_deadline_ms != 0 && now_ms >= s_deadline_ms) {
      set_error(ctx, "応答がありませんでした");
    }
    return;
  }
  if (s_phase == AgentPhase::Thinking) {
    // 送信/返答待ちの間に BLE が切れたらエラー。
    if (ctx.power && !ctx.power->ble_connected()) {
      set_error(ctx, "スマホとつながっていません");
      return;
    }
    // 全体のタイムアウト (停止送信から60秒)。
    if (s_deadline_ms != 0 && now_ms >= s_deadline_ms) {
      set_error(ctx, "応答がありませんでした");
    }
  }
}

int64_t next_deadline_ms() {
  int64_t d = 0;
  if (s_phase == AgentPhase::Sending || s_phase == AgentPhase::Thinking) {
    d = s_deadline_ms;
  }
  if (s_phase == AgentPhase::Recording || s_phase == AgentPhase::Sending) {
    if (s_pump_deadline_ms != 0 &&
        (d == 0 || s_pump_deadline_ms < d)) {
      d = s_pump_deadline_ms;
    }
  }
  return d;
}

}  // namespace

AgentPhase agent_phase() { return s_phase; }
const char* agent_reply_text() { return s_reply; }
const char* agent_error_text() { return s_error; }
bool agent_recording() { return s_phase == AgentPhase::Recording; }
const char* agent_partial_text() { return s_partial; }
bool agent_partial_final() { return s_partial_final; }
bool agent_dictating() { return s_stream_mode; }
const char* agent_notice_text() { return s_notice; }

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
  if (!out || s_phase != AgentPhase::Sending || !s_has_pending_text) return false;
  out->id = s_req_id;
  std::memcpy(out->text, s_pending_text, sizeof(s_pending_text));
  return true;
}

void agent_send_started(uint16_t id) {
  if (s_phase != AgentPhase::Sending || id != s_req_id) return;
  s_has_pending_text = false;
}

void agent_sent(uint16_t id, bool ok, FeatureContext& ctx) {
  // 定型質問の EVT を出した (ok) / 出せなかった (!ok)。
  if (s_phase != AgentPhase::Sending || id != s_req_id) return;
  s_has_pending_text = false;
  if (ok) {
    set_phase(ctx, AgentPhase::Thinking);
  } else {
    set_error(ctx, "送信に失敗しました");
  }
}

bool agent_stream_pending(AgentStreamRequest* out) {
  if (!out || s_stream_cmd == AgentStreamCmd::None) return false;
  if (s_phase != AgentPhase::Recording && s_phase != AgentPhase::Sending) {
    return false;
  }
  out->cmd = s_stream_cmd;
  out->id = s_stream_id;
  return true;
}

void agent_stream_started(uint16_t id) {
  if (id == 0 || id != s_stream_id) return;
  if (s_phase != AgentPhase::Recording && s_phase != AgentPhase::Sending) {
    return;
  }
  if (s_stream_cmd == AgentStreamCmd::Start) {
    s_stream_cmd = AgentStreamCmd::None;
  }
  s_stream_started = true;
}

void agent_stream_acked(uint16_t id) {
  if (id == 0 || id != s_stream_id) return;
  // 旧アプリ検出の警告を解除する (遅れて ACK するスマホを誤判定しない)。
  s_notice[0] = '\0';
}

void agent_stream_stopped(uint16_t id, FeatureContext& ctx) {
  if (s_phase != AgentPhase::Sending || id == 0 || id != s_req_id) return;
  s_stream_cmd = AgentStreamCmd::None;
  s_pump_deadline_ms = 0;
  if (ctx.audio) ctx.audio->record_stream_enable(false);  // リング解放
  if (s_after_stop_error[0]) {
    set_error(ctx, s_after_stop_error);
    return;
  }
  // タイムアウトは停止送信の完了時点から数え直す。
  s_deadline_ms = ctx.clock.now_ms() + kAgentTimeoutMs;
  set_phase(ctx, AgentPhase::Thinking);
}

void agent_stream_failed(uint16_t id, bool unsupported, FeatureContext& ctx) {
  if (id == 0 || id != s_stream_id) return;
  if (s_phase == AgentPhase::Recording) {
    // 仕様: 旧アプリ (dictation 非対応) は聞き取りを継続しつつ警告を出し、
    // 聞き取り終了時にエラーにする。ACK が後から来たら agent_stream_acked で消す。
    if (s_stream_cmd == AgentStreamCmd::Start) {
      s_stream_cmd = AgentStreamCmd::None;
      s_stream_started = false;
    }
    std::snprintf(s_notice, sizeof(s_notice), "%s",
                  unsupported ? "スマホを更新してください"
                              : "音声を送信できません");
    // 状態は変えないが、警告の表示更新のため Event を出す。
    ctx.bus.publish(
        {EventType::AgentStatusChanged, static_cast<uint32_t>(s_phase)});
    return;
  }
  if (s_phase == AgentPhase::Sending) {
    s_stream_cmd = AgentStreamCmd::None;
    set_error(ctx, unsupported ? "スマホを更新してください"
                               : "スマホとつながっていません");
  }
}

bool agent_on_reply(uint16_t id, const char* text, size_t len,
                    FeatureContext& ctx) {
  if (!text || id != s_req_id) return false;
  // BULK_END を送ってから Thinking へ落ちるまでの競合で返答が先着しても
  // 落とさない (id は聞き取り開始時に採番済み)。
  if (s_phase == AgentPhase::Sending && s_stream_mode && s_stream_started &&
      s_stream_cmd == AgentStreamCmd::None) {
    if (ctx.audio) ctx.audio->record_stream_enable(false);
    s_pump_deadline_ms = 0;
    set_phase(ctx, AgentPhase::Thinking);
  }
  if (s_phase != AgentPhase::Thinking) return false;
  copy_utf8(s_reply, sizeof(s_reply), text, len);
  if (ctx.power) ctx.power->kick_activity(ctx.clock.now_ms());
  set_phase(ctx, AgentPhase::Reply);
  return true;
}

bool agent_on_partial(uint16_t id, const char* text, size_t len, bool final,
                      FeatureContext& ctx) {
  if (!text || id == 0 || id != s_req_id) return false;
  if (s_phase != AgentPhase::Recording && s_phase != AgentPhase::Sending &&
      s_phase != AgentPhase::Thinking) {
    return false;
  }
  // 確定後の途中結果 (順序入れ替わり) は無視する。
  if (s_partial_final && !final) return false;
  copy_utf8(s_partial, sizeof(s_partial), text, len);
  if (final) s_partial_final = true;
  if (ctx.power) ctx.power->kick_activity(ctx.clock.now_ms());
  ctx.bus.publish({EventType::AgentPartialChanged, 0});
  return true;
}

void agent_reset_state() {
  s_phase = AgentPhase::Idle;
  clear_request();
  s_next_id = 1;
  s_has_pending_text = false;
  s_pending_text[0] = '\0';
  s_partial[0] = '\0';
  s_partial_final = false;
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
