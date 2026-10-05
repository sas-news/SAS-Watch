// sim_platform.cpp — core の platform.hpp を Linux で実装するスタブ。
// 時刻は deterministic に動かせる (スナップショットを再現可能にするため)。
#include "sim_platform.hpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace sim {

// ---- KV: メモリ上の map (永続化はスナップショットに不要) ----------------------
class SimKv : public watch::KeyValueStore {
 public:
  size_t size(const char* key) override {
    auto it = m_.find(key);
    return it == m_.end() ? 0 : it->second.size();
  }
  bool get(const char* key, void* buf, size_t cap, size_t* out) override {
    auto it = m_.find(key);
    if (it == m_.end()) return false;
    if (it->second.size() > cap) {
      if (out) *out = it->second.size();
      return false;
    }
    std::memcpy(buf, it->second.data(), it->second.size());
    if (out) *out = it->second.size();
    return true;
  }
  bool set(const char* key, const void* data, size_t len) override {
    if (len == 0) {
      m_.erase(key);
      return true;
    }
    m_[key] = std::vector<uint8_t>(static_cast<const uint8_t*>(data),
                                   static_cast<const uint8_t*>(data) + len);
    return true;
  }
  bool erase(const char* key) override {
    m_.erase(key);
    return true;
  }

 private:
  std::map<std::string, std::vector<uint8_t>> m_;
};

class SimLog : public watch::Log {
 public:
  void write(watch::LogLevel level, const char* tag,
             const char* message) override {
    static const char* names[] = {"D", "I", "W", "E"};
    std::printf("[%s] %s: %s\n", names[static_cast<int>(level)], tag,
                message);
  }
};

static SimClock s_clock;
static SimKv s_kv;
static SimLog s_log;

SimClock& clock() { return s_clock; }
watch::KeyValueStore& kv() { return s_kv; }
watch::Log& log() { return s_log; }

}  // namespace sim
