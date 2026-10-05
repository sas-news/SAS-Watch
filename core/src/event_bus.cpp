#include "watch/event_bus.hpp"

namespace watch {

bool EventBus::subscribe(EventType type, EventHandler fn, void* ctx) {
  if (fn == nullptr) return false;
  for (size_t i = 0; i < count_; ++i) {
    if (subs_[i].type == type && subs_[i].fn == fn && subs_[i].ctx == ctx) {
      return true;  // 重複は無視
    }
  }
  if (count_ >= kMaxSubscriptions) return false;
  subs_[count_] = {type, fn, ctx};
  ++count_;
  return true;
}

void EventBus::unsubscribe(EventHandler fn, void* ctx) {
  size_t i = 0;
  while (i < count_) {
    if (subs_[i].fn == fn && subs_[i].ctx == ctx) {
      subs_[i] = subs_[count_ - 1];
      --count_;
    } else {
      ++i;
    }
  }
}

void EventBus::publish(const Event& e) {
  if (publishing_) return;  // ハンドラ内からの再入は防ぐ
  publishing_ = true;
  // ハンドラが unsubscribe しても配列は詰められるだけなので、
  // index で回すと抜けが出る。先に有効な (fn,ctx) を読んでから呼ぶ。
  for (size_t i = 0; i < count_; ++i) {
    const Subscription s = subs_[i];
    if (s.fn != nullptr && (s.type == e.type || s.type == EventType::None)) {
      s.fn(e, s.ctx);
    }
  }
  publishing_ = false;
}

size_t EventBus::count() const { return count_; }

}  // namespace watch
