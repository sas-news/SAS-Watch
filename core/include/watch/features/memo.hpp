// features/memo.hpp — テキスト (最大64件×256B) と音声 (最大8件・合計1MB) のメモ。
// テキストは KeyValueStore、音声の実体は AudioPort 側のストレージ (littlefs)。
// 溢れたら古いものから消す。
#pragma once

#include "watch/feature.hpp"

namespace watch {
namespace features {

constexpr size_t kMemoMaxEntries = 64;
constexpr size_t kMemoMaxText = 256;
constexpr size_t kMemoMaxVoice = 8;                    // 音声メモの最大件数
constexpr uint32_t kMemoVoiceMaxSec = 60;              // 1件の最大秒数
constexpr uint32_t kMemoVoiceBudgetBytes = 1u << 20;   // 音声の合計バイト上限

enum class MemoKind : uint8_t { Text = 0, Voice = 1 };

struct MemoEntry {
  uint32_t id = 0;
  MemoKind kind = MemoKind::Text;
  uint16_t len = 0;    // テキストのバイト数 (Voice では 0)
  uint32_t sec = 0;    // 音声の長さ (秒)
  uint32_t size = 0;   // 音声ファイルのバイト数
  char text[kMemoMaxText] = {};  // NUL 終端保証
};

extern const FeatureDescriptor kMemo;

// UI の Presenter が使う const アクセサ。
size_t memo_count();
// i は 0 が最古。範囲外は false。
bool memo_at(size_t i, MemoEntry* out);
// id で引く。無ければ false。
bool memo_find(uint32_t id, MemoEntry* out);

// 直接の作成 API (protocol の memo.create が使う)。id>=0、失敗は -1。
int32_t memo_create(const char* text, size_t len, FeatureContext& ctx);

// id のメモを消す。無ければ false (UI の削除アクションが使う)。
// Voice なら実ファイルも消す。
bool memo_delete(uint32_t id, FeatureContext& ctx);

// ---- 録音/再生 (UI と memo.record Action が使う) ----
// 録音中なら停止して確定、止まっていれば開始。開始/確定した id、失敗は -1。
int32_t memo_record_toggle(FeatureContext& ctx);
// 録音中か (UI の録音表示用)。
bool memo_recording();
uint32_t memo_record_id();  // 録音中の id (無ければ 0)
uint32_t memo_record_elapsed_s(FeatureContext& ctx);
uint8_t memo_record_level(FeatureContext& ctx);

// 再生中の id (0=なし)。再生は排他: 別のを始めると前は止まる。
uint32_t memo_playing_id();
// Voice メモの再生を始める。volume は Settings から取る。失敗は false。
bool memo_play(uint32_t id, FeatureContext& ctx);
void memo_stop_play(FeatureContext& ctx);

// 内部状態を初期値に戻す (主にテストと工場リセット用)。KV は消さない。
void memo_reset_state();

}  // namespace features
}  // namespace watch
