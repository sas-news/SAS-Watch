// runtime.hpp — ActionQueue (固定長リング) と Runtime。
// ISR/他タスクから push する想定。スレッド安全化はプラットフォーム側の
// lock/unlock フックで差し込む (core は FreeRTOS を知らない)。
#pragma once

#include <cstdint>

#include "watch/action.hpp"
#include "watch/event_bus.hpp"
#include "watch/feature.hpp"
#include "watch/input_mapper.hpp"
#include "watch/navigation.hpp"
#include "watch/power.hpp"
#include "watch/settings.hpp"

namespace watch {

class ActionQueue {
 public:
  static constexpr size_t kCapacity = 16;
  using LockFn = void (*)(void* ctx);

  // ISR/他タスクから呼ぶ。満杯なら false (落とす)。
  bool push(const Action& a);
  // app タスクから呼ぶ。空なら false。
  bool pop(Action* out);
  bool empty() const { return count_ == 0; }
  size_t count() const { return count_; }

  // スレッド安全化フック (例: portMUX_TYPE / mutex)。nullptr で無効。
  void set_lock_hooks(LockFn lock, LockFn unlock, void* ctx) {
    lock_ = lock;
    unlock_ = unlock;
    lock_ctx_ = ctx;
  }

 private:
  Action buf_[kCapacity] = {};
  size_t head_ = 0;   // 次に読む位置
  size_t count_ = 0;
  LockFn lock_ = nullptr;
  LockFn unlock_ = nullptr;
  void* lock_ctx_ = nullptr;
};

// core 全体の司令塔。app タスクから1本ずつ step() する想定。
class Runtime {
 public:
  Runtime(EventBus& bus, Navigator& nav, PowerPolicy& power,
          InputMapper& input, const FeatureRegistry& features,
          Settings& settings);

  // Feature を init する (保存データの restore は呼ばない)。
  void init(FeatureContext& ctx);

  // Action を1件処理 + 全 Feature の tick + PowerPolicy update。
  // Action が無くても tick は回す。Action を処理したら true。
  bool step(int64_t now_ms, FeatureContext& ctx);

  // 次に起きる必要がある時刻 (Feature deadline と電源遷移の最小値)。
  // 0 = 期限なし = 無制限に寝てよい。
  int64_t next_deadline_ms() const;

  ActionQueue& queue() { return queue_; }
  const FeatureRegistry& features() const { return features_; }

 private:
  bool dispatch(const Action& a, FeatureContext& ctx);

  EventBus& bus_;
  Navigator& nav_;
  PowerPolicy& power_;
  InputMapper& input_;
  const FeatureRegistry& features_;
  Settings& settings_;
  ActionQueue queue_;
};

}  // namespace watch
