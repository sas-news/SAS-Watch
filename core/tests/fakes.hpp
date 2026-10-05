// テスト用フェイク。PC 専用なので STL を使ってよい。
#pragma once

#include <map>
#include <string>
#include <vector>

#include "watch/platform.hpp"

namespace watch {
namespace test {

class FakeClock : public Clock {
 public:
  int64_t now_ms() override { return now_; }
  int64_t epoch_s() override { return epoch_; }
  bool set_epoch_s(int64_t s) override {
    epoch_ = s;
    ++set_calls;
    return true;
  }
  void advance_ms(int64_t d) {
    now_ += d;
    epoch_ += d / 1000;  // 雑な近似: 壁時計も一緒に進める
  }
  void set_now_ms(int64_t v) { now_ = v; }

  int set_calls = 0;

 private:
  int64_t now_ = 0;
  int64_t epoch_ = 1'700'000'000;
};

class MemoryKeyValueStore : public KeyValueStore {
 public:
  size_t size(const char* key) override {
    auto it = data_.find(key);
    return it == data_.end() ? 0 : it->second.size();
  }
  bool get(const char* key, void* buf, size_t cap, size_t* out_len) override {
    auto it = data_.find(key);
    if (it == data_.end()) return false;
    if (out_len) *out_len = it->second.size();
    if (it->second.size() > cap) return false;
    std::memcpy(buf, it->second.data(), it->second.size());
    return true;
  }
  bool set(const char* key, const void* data, size_t len) override {
    const auto* p = static_cast<const uint8_t*>(data);
    data_[key] = std::vector<uint8_t>(p, p + len);
    return true;
  }
  bool erase(const char* key) override { return data_.erase(key) > 0; }

  const std::map<std::string, std::vector<uint8_t>>& data() const {
    return data_;
  }

 private:
  std::map<std::string, std::vector<uint8_t>> data_;
};

// publish された Event を順に記録するハンドラ。
struct EventRecorder {
  std::vector<Event> events;

  static void handler(const Event& e, void* ctx) {
    static_cast<EventRecorder*>(ctx)->events.push_back(e);
  }
  void attach(EventBus& bus, EventType type) {
    bus.subscribe(type, &EventRecorder::handler, this);
  }
  void attach_all(EventBus& bus) { attach(bus, EventType::None); }
  size_t count_of(EventType t) const {
    size_t n = 0;
    for (const auto& e : events) {
      if (e.type == t) ++n;
    }
    return n;
  }
  const Event* last_of(EventType t) const {
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
      if (it->type == t) return &*it;
    }
    return nullptr;
  }
};

}  // namespace test
}  // namespace watch
