// features/muse.hpp — Muse の返答を字幕表示するアプリ (Phase 2 ⑤, T56)。
//
// 接続・送受信は firmware の MusePort 実装 (muse_link) に任せ、core は
// 「状態スナップショット」と「返答のページ管理」だけを持つ。
// 返答は kMuseReplyMax (960B) まで取り込み、kMusePageBytes ごとのページに
// UTF-8 境界で分割して、kMusePageMs (15 秒) ごとに自動で次のページへ送る。
// 画面タップ/「次へ」でも次ページ (最後まで行ったら先頭へ戻る)。
//
// 音声はこの UI ではまだ送らない (muse_link_send_voice_note は API のみ)。
// 返答本文はログ/シリアルに出さない (画面にだけ出す)。
#pragma once

#include "watch/feature.hpp"
#include "watch/muse.hpp"

namespace watch {
namespace features {

constexpr size_t kMuseReplyMax = 960;   // 表示する返答の上限 (agent.reply と同じ)
constexpr size_t kMusePageBytes = 300;  // 1ページの目安バイト数 (UTF-8 境界で切る)
constexpr int64_t kMusePageMs = 15000;  // 自動ページ送りの間隔
constexpr size_t kMuseErrorMax = 96;    // 日本語エラーの表示上限
constexpr uint16_t kMuseMaxPages = 8;   // ページ開始位置の保持数 (960B/300B で最大 4)

extern const FeatureDescriptor kMuse;

// ---- 画面 (LVGL タスク) 用のアクセサ ----
bool muse_available();          // port があり、その個体で使えるか
MuseState muse_state();
const char* muse_error();       // 空なら ""
uint16_t muse_page();           // 現在ページ (0 始まり)
uint16_t muse_pages();          // 総ページ数 (返答なし = 0)
uint32_t muse_reply_revision(); // 返答が更新されるたびに増える
// 現在ページの本文を out にコピー (NUL 終端)。返答が無ければ false。
bool muse_page_text(char* out, size_t cap);

// 次のページへ (タップ/「次へ」ボタン)。最後まで行ったら先頭へ戻る。
void muse_next_page();

// ---- 操作 (Feature handle / 画面ボタンから) ----
void muse_request_start(FeatureContext& ctx);  // 未接続なら接続
void muse_request_stop(FeatureContext& ctx);   // 接続中なら切断
bool muse_request_send_text(FeatureContext& ctx, const char* text);

// 内部状態を初期値に戻す (テスト用)。
void muse_reset_state();

}  // namespace features
}  // namespace watch
