// features/notify.hpp — 通知一覧 (最新20件をリング保持。RAMのみ、永続化しない)。
// スマホの notify.post で届く。受信時のポップアップ・振動は UI 層の仕事。
#pragma once

#include "watch/feature.hpp"

namespace watch {
namespace features {

constexpr size_t kNotifyMax = 20;
constexpr size_t kNotifyAppMax = 48;
constexpr size_t kNotifyTitleMax = 96;
constexpr size_t kNotifyBodyMax = 256;

struct NotifyEntry {
  char app[kNotifyAppMax] = {};
  char title[kNotifyTitleMax] = {};
  char body[kNotifyBodyMax] = {};
};

extern const FeatureDescriptor kNotify;

size_t notify_count();
// i = 0 が最新。範囲外は false。
bool notify_at(size_t i, NotifyEntry* out);
// 受信時に呼ぶ (protocol notify.post 経由)。NUL 終端された文字列を渡す。
void notify_add(const char* app, const char* title, const char* body);
// 「すべて消す」。NotificationsCleared を出す。
void notify_clear(FeatureContext& ctx);

void notify_reset_state();

}  // namespace features
}  // namespace watch
