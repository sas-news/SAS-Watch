// event_bus.hpp — 固定長購読表。同期 publish。malloc なし。
// plan.md C章のコード例に準拠。
#pragma once

#include <array>
#include <cstddef>

#include "watch/event.hpp"

namespace watch {

using EventHandler = void (*)(const Event&, void* ctx);

struct Subscription {
  EventType type = EventType::None;
  EventHandler fn = nullptr;
  void* ctx = nullptr;
};

class EventBus {
 public:
  static constexpr size_t kMaxSubscriptions = 48;

  // 同じ (type, fn, ctx) の重複登録は無視する。満杯なら false。
  bool subscribe(EventType type, EventHandler fn, void* ctx);
  // fn/ctx が一致する購読を全部外す。
  void unsubscribe(EventHandler fn, void* ctx);
  // 購読者へ同期で配る。app タスク内から呼ぶ想定。
  // ハンドラ内で publish する再入は許さない (ガード付き)。
  void publish(const Event& e);
  size_t count() const;

 private:
  std::array<Subscription, kMaxSubscriptions> subs_{};
  size_t count_ = 0;
  bool publishing_ = false;
};

}  // namespace watch
