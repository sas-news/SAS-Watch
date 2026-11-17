// features/agent.hpp — 時計から AI に話しかける (Phase Agent / T38)。
// 時計はオフライン。「話しかける」で音声入力を始めると同時に、16kHz IMA ADPCM を
// BULK kind="dictation" でスマホへリアルタイム送出する (ble_glue が送出し)。
// **音声は保存しない** (時計側に音声ファイル/永続バッファは作らない)。
// スマホがその場で文字起こしし、部分結果を REQ agent.partial で返すので、
// 時計は聞き取り中に表示する。確定/応答は agent.partial{final:true} と
// agent.reply。話した内容は部分文字起こしと Muse へのテキストだけが残る。
// 定型質問 (settings agent.q1..3) は EVT agent.request でテキストを送る。
// 状態遷移は AgentStatusChanged Event で UI に知らせる。
#pragma once

#include "watch/feature.hpp"

namespace watch {
namespace features {

// 1回の音声入力の上限秒数。音声は保存しない (ストリーミングのみ) ので、
// 発話時間の制限ではなく暴走・止め忘れ防止の安全弁 (3分)。
constexpr uint32_t kAgentMaxRecordSec = 180;
constexpr int64_t kAgentTimeoutMs = 60'000;     // 停止送信から返答までの上限
constexpr size_t kAgentReplyMax = 960;          // agent.reply text の最大バイト数
constexpr size_t kAgentPartialMax = 768;        // agent.partial text の最大バイト数
constexpr size_t kAgentQuestionCount = 3;       // 定型質問ボタンの最大数
constexpr size_t kAgentQuestionMax = 63;        // 1問の最大バイト数 (settings Str 上限)
// 音声入力ジョブ用の仮 id。メモの id 空間 (小さい整数) と衝突しない値。
// dictation 中はファイルを一切作らない (PSRAM の録音ストリームへ流すだけ)
// が、AudioPort::record_begin が id を要求するので固定値を使う。
constexpr uint32_t kAgentAudioFileId = 0x80000000u;
// dictation ストリームのポーリング周期 (app タスクをこの間隔で起こす)。
// TODO(hw): 実機で 8KB/s を維持できる周期 (20ms は目安) を確認する。
constexpr int64_t kAgentPumpMs = 20;

// 画面表示用の状態。AgentStatusChanged の arg0 にも使う。
enum class AgentPhase : uint8_t {
  Idle = 0,      // 待機
  Recording,     // 聞き取り中 (BULK kind="dictation" で音声を送出し、部分表示)
  Sending,       // 聞き取り停止。残りを吐いて BULK_END を送る (またはテキスト送出)
  Thinking,      // 送信完了、確定テキスト/返答待ち
  Reply,         // 返答を表示中
  Error,         // エラーを表示中
};

// 定型質問 (EVT agent.request) の送信待ち。ble_glue が拾う。
struct AgentPending {
  uint16_t id = 0;                   // agent.reply の照合用 id
  char text[kAgentQuestionMax + 1] = {};  // EVT agent.request の本文
};

// dictation ストリームの開始/停止要求。ble_glue が app タスクで拾う。
enum class AgentStreamCmd : uint8_t {
  None = 0,
  Start,  // BULK_START kind="dictation" を送る (id = talk id)
  Stop,   // 残りを吐き切って BULK_END を送る
};

struct AgentStreamRequest {
  AgentStreamCmd cmd = AgentStreamCmd::None;
  uint16_t id = 0;  // talk id (agent.partial / agent.reply と同値)
};

extern const FeatureDescriptor kAgent;

// ---- 画面/presenter 用の const アクセサ ----
AgentPhase agent_phase();
const char* agent_reply_text();   // phase==Reply の本文 (空なら "")
const char* agent_error_text();   // phase==Error のメッセージ
bool agent_recording();           // phase==Recording
uint32_t agent_record_elapsed_s(FeatureContext& ctx);
uint8_t agent_record_level(FeatureContext& ctx);
// i 番目の定型質問 (settings agent.q<i+1>)。空/未設定なら nullptr。
const char* agent_question(size_t i, const Settings* s);
// 現在の部分文字起こし (CUMULATIVE)。無ければ ""。
const char* agent_partial_text();
// 部分文字起こしが確定済み (final:true) か。
bool agent_partial_final();
// 現在の要求が dictation (音声入力) か。false なら定型質問 (テキスト)。
bool agent_dictating();
// 聞き取り中の警告 (旧アプリ検出など)。無ければ ""。
const char* agent_notice_text();

// ---- 定型質問の送信側 (ble_glue が app タスクのループで呼ぶ) ----
// 送りたい物があれば out を埋めて true。呼び出し側が実際に送り始めたら
// agent_send_started(id) を呼んで pending を閉じる (再送出し防止)。
// EVT の送出し完了は agent_sent(id, ok) で知らせる
// (ok で Thinking、失敗で Error)。
bool agent_pending(AgentPending* out);
void agent_send_started(uint16_t id);
void agent_sent(uint16_t id, bool ok, FeatureContext& ctx);

// ---- dictation ストリームの送信側 (ble_glue が app タスクのループで呼ぶ) ----
// 開始/停止要求があれば out を埋めて true (応答が済むまで毎ループ true)。
bool agent_stream_pending(AgentStreamRequest* out);
// BULK_START kind="dictation" を送った。
void agent_stream_started(uint16_t id);
// 最初の BULK_ACK を受けた (旧アプリ検出の警告を解除する)。
void agent_stream_acked(uint16_t id);
// 残りを吐き切って BULK_END を送った → Thinking へ。
void agent_stream_stopped(uint16_t id, FeatureContext& ctx);
// ストリームを開始/継続できない。
//   unsupported=true : スマホが dictation 非対応 (旧アプリ / bulk 未購読)。
//                      聞き取り中なら警告を出して聞き取りは継続し、停止時に Error。
//   false            : 切断などで送出し不能 → Error。
void agent_stream_failed(uint16_t id, bool unsupported, FeatureContext& ctx);

// ---- 受信側 (dispatch の agent.reply / agent.partial が呼ぶ) ----
// id が現在の要求と一致して Thinking なら本文を受け取って Reply にする。
// 適用できたら true (適用不可でも呼び出し側は RES ok でよい)。
bool agent_on_reply(uint16_t id, const char* text, size_t len,
                    FeatureContext& ctx);
// 部分文字起こし。Recording/Sending/Thinking かつ id 一致のときだけ反映する
// (CUMULATIVE: 前回を置き換える)。final=true で確定。空文字はクリア。
bool agent_on_partial(uint16_t id, const char* text, size_t len, bool final,
                      FeatureContext& ctx);

// 内部状態を初期値に戻す (テスト用)。
void agent_reset_state();

}  // namespace features
}  // namespace watch
