// features/agent.hpp — 時計から AI に話しかける (Phase Agent)。
// 時計はオフライン。録音 (ADP1) は BULK kind="agent" でスマホへ送り、
// スマホが STT → LLM に投げて agent.reply REQ で返答を返す。
// 定型質問 (settings agent.q1..3) は EVT agent.request でテキストを送る。
// 状態遷移は AgentStatusChanged Event で UI に知らせる。
#pragma once

#include "watch/feature.hpp"

namespace watch {
namespace features {

constexpr uint32_t kAgentMaxRecordSec = 30;     // 1回の録音の最大秒数
constexpr int64_t kAgentTimeoutMs = 60'000;     // 送信開始から返答までの上限
constexpr size_t kAgentReplyMax = 960;          // agent.reply text の最大バイト数
constexpr size_t kAgentQuestionCount = 3;       // 定型質問ボタンの最大数
constexpr size_t kAgentQuestionMax = 63;        // 1問の最大バイト数 (settings Str 上限)
// 録音ファイルの仮 id。メモの id 空間 (小さい整数) と衝突しない値。
constexpr uint32_t kAgentAudioFileId = 0x80000000u;

// 画面表示用の状態。AgentStatusChanged の arg0 にも使う。
enum class AgentPhase : uint8_t {
  Idle = 0,      // 待機
  Recording,     // 録音中
  Sending,       // 音声/質問を送信中 (BLE 未接続なら再試行待ち)
  Thinking,      // 送信完了、返答待ち
  Reply,         // 返答を表示中
  Error,         // エラーを表示中
};

// ble_glue が拾う「送りたい物」。audio=true なら file_id の音声を BULK
// kind="agent" で、false なら text を EVT agent.request で送る。
struct AgentPending {
  bool audio;
  uint16_t id;        // agent.reply の照合用 id (BULK transfer id と同じ)
  uint32_t file_id;   // audio のみ
  char text[kAgentQuestionMax + 1];  // text のみ
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

// ---- 送信側 (ble_glue が app タスクのループで呼ぶ) ----
// 送りたい物があれば out を埋めて true。呼び出し側が実際に送り始めたら
// agent_send_started(id) を呼んで pending を閉じる (再送出し防止)。
// BULK の完了/失敗や EVT の送出し完了は agent_sent(id, ok) で知らせる
// (ok で Thinking、失敗で Error に遷移して音声ファイルを消す)。
// 送出しに失敗したまま pending を閉じなければ次のループで再試行される
// (全体のタイムアウトは kAgentTimeoutMs)。
bool agent_pending(AgentPending* out);
void agent_send_started(uint16_t id);
void agent_sent(uint16_t id, bool ok, FeatureContext& ctx);

// ---- 受信側 (dispatch の agent.reply が呼ぶ) ----
// id が現在の要求と一致して Thinking なら本文を受け取って Reply にする。
// 適用できたら true (適用不可でも呼び出し側は RES ok でよい)。
bool agent_on_reply(uint16_t id, const char* text, size_t len,
                    FeatureContext& ctx);

// 内部状態を初期値に戻す (テスト用)。
void agent_reset_state();

}  // namespace features
}  // namespace watch
