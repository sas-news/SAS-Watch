// features/memo.hpp — 最大64件×最大256バイト(UTF-8)のメモ。
// リングバッファで、溢れたら古いものから消す。KeyValueStore に永続化。
#pragma once

#include "watch/feature.hpp"

namespace watch {
namespace features {

constexpr size_t kMemoMaxEntries = 64;
constexpr size_t kMemoMaxText = 256;

struct MemoEntry {
  uint32_t id = 0;
  uint16_t len = 0;
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

// 内部状態を初期値に戻す (主にテストと工場リセット用)。KV は消さない。
void memo_reset_state();

}  // namespace features
}  // namespace watch
