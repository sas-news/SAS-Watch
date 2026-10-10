// Notify Feature。スマホからの通知を RAM リング (20件) に保持する。
// 永続化しない (起きた時計に追いつかない通知は諦める)。
#include "watch/features/notify.hpp"

#include <cstring>

namespace watch {
namespace features {

namespace {

NotifyEntry g_entries[kNotifyMax];
uint8_t g_head = 0;    // 最新スロット (次に書く位置)
uint8_t g_count = 0;

void copy_str(char* dst, size_t cap, const char* s) {
  if (!s) {
    dst[0] = '\0';
    return;
  }
  size_t n = std::strlen(s);
  if (n >= cap) n = cap - 1;
  std::memcpy(dst, s, n);
  dst[n] = '\0';
}

bool handle(const Action& a, FeatureContext& ctx) {
  if (a.type != ActionType::NotifyClearAll) return false;
  g_head = 0;
  g_count = 0;
  for (auto& e : g_entries) e = NotifyEntry{};
  ctx.bus.publish({EventType::NotificationsCleared, 0});
  return true;
}

}  // namespace

size_t notify_count() { return g_count; }

bool notify_at(size_t i, NotifyEntry* out) {
  if (!out || i >= g_count) return false;
  // 0 が最新: head-1 が最新、head-2 がその前…
  const int idx = static_cast<int>(g_head) - 1 - static_cast<int>(i);
  *out = g_entries[(idx + kNotifyMax) % kNotifyMax];
  return true;
}

void notify_add(const char* app, const char* title, const char* body) {
  NotifyEntry& e = g_entries[g_head];
  copy_str(e.app, sizeof(e.app), app);
  copy_str(e.title, sizeof(e.title), title);
  copy_str(e.body, sizeof(e.body), body);
  g_head = static_cast<uint8_t>((g_head + 1) % kNotifyMax);
  if (g_count < kNotifyMax) ++g_count;
  // NotificationPosted は呼び出し側 (dispatch) が publish する。
}

void notify_clear(FeatureContext& ctx) {
  Action a{};
  a.type = ActionType::NotifyClearAll;
  handle(a, ctx);
}

void notify_reset_state() {
  g_head = 0;
  g_count = 0;
  for (auto& e : g_entries) e = NotifyEntry{};
}

const FeatureDescriptor kNotify = {
    /*id*/ "notify",
    /*capabilities*/ HasScreen | HasQuickTile,
    /*route*/ Route::Notifications,
    /*init*/ nullptr,
    /*handle*/ handle,
    /*tick*/ nullptr,
    /*next_deadline_ms*/ nullptr,
    /*save*/ nullptr,
    /*restore*/ nullptr,
};

}  // namespace features
}  // namespace watch
