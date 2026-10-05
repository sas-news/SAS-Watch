// feature.hpp — Feature の静的 Registry。plan.md E章。
// Feature は画面を持たない。State は Feature ごと、Event で知らせる。
#pragma once

#include <cstdint>

#include "watch/action.hpp"
#include "watch/event_bus.hpp"
#include "watch/navigation.hpp"
#include "watch/platform.hpp"

namespace watch {

class PowerPolicy;

// Feature が共有するコンテキスト。実体は Runtime が持つ。
struct FeatureContext {
  EventBus& bus;
  KeyValueStore& storage;
  Clock& clock;
  Navigator* nav = nullptr;      // 無くても動く (テスト用に nullable)
  PowerPolicy* power = nullptr;  // 同上
};

enum FeatureCapability : uint32_t {
  CapNone = 0,
  HasScreen = 1u << 0,      // Route を持つ画面がある
  HasHomeWidget = 1u << 1,  // Home のチップ表示
  HasQuickTile = 1u << 2,   // Quick から起動
  NeedsAudio = 1u << 3,
};

struct FeatureDescriptor {
  const char* id;            // "timer" など (settings の key にも使う)
  uint32_t capabilities;     // FeatureCapability の OR
  Route route;               // 画面を持つならその Route。無ければ Route::None
  void (*init)(FeatureContext&) = nullptr;
  // Action を処理したら true。全部の Feature に順に聞く。
  bool (*handle)(const Action&, FeatureContext&) = nullptr;
  // app タスクの1ループごとに呼ぶ (期限チェックなど)。
  void (*tick)(int64_t now_ms, FeatureContext&) = nullptr;
  // 次に起きる必要がある monotonic 時刻。0 = なし。
  int64_t (*next_deadline_ms)() = nullptr;
  // Deep Sleep 前の保存/復帰時の復元。ctx.storage に書き、
  // 時刻が要るなら ctx.clock を使う (Timer)。無ければ nullptr。
  void (*save)(FeatureContext&) = nullptr;
  void (*restore)(FeatureContext&) = nullptr;
};

// 静的 Registry。実行時プラグインは作らない (plan.md E章)。
class FeatureRegistry {
 public:
  FeatureRegistry(const FeatureDescriptor* const* descs, size_t n)
      : descs_(descs), count_(n) {}

  size_t count() const { return count_; }
  const FeatureDescriptor* at(size_t i) const { return descs_[i]; }
  const FeatureDescriptor* find(const char* id) const;
  // Route を持つ Feature を引く。無ければ nullptr。
  const FeatureDescriptor* find_by_route(Route r) const;

  void init_all(FeatureContext& ctx) const;
  // 順に聞いて最初に true を返したところで止める。
  bool handle(const Action& a, FeatureContext& ctx) const;
  void tick_all(int64_t now_ms, FeatureContext& ctx) const;
  // 最小の deadline (0 以外)。なければ 0。
  int64_t next_deadline_ms() const;
  void save_all(FeatureContext& ctx) const;
  void restore_all(FeatureContext& ctx) const;

 private:
  const FeatureDescriptor* const* descs_;
  size_t count_;
};

// ビルドに含まれる Feature の一覧 (features/registry.cpp)。
const FeatureDescriptor* const* builtin_features();
size_t builtin_features_count();

}  // namespace watch
