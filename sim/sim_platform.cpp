// sim_platform.cpp — core の platform.hpp を Linux で実装するスタブ。
// 時刻は deterministic に動かせる (スナップショットを再現可能にするため)。
#include "sim_platform.hpp"

#include <algorithm>
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

// ---- Audio: 録音/再生をメモリで真似る (スナップショットの録音表示用) --------
class SimAudio : public watch::AudioPort {
 public:
  bool record_begin(uint32_t memo_id, uint32_t) override {
    if (recording_ || playing_) return false;
    recording_ = true;
    rec_id_ = memo_id;
    rec_start_ms_ = s_clock.now_ms();
    return true;
  }
  bool record_end(bool commit, uint32_t* out_sec,
                  uint32_t* out_size) override {
    if (!recording_) return false;
    const uint32_t sec = record_elapsed_s();
    recording_ = false;
    const uint32_t size = 16 + sec * 8000;
    if (commit) {
      std::vector<uint8_t> blob(size, 0);
      blob[0] = 'A'; blob[1] = 'D'; blob[2] = 'P'; blob[3] = '1';
      for (size_t i = 16; i < size; ++i) {
        blob[i] = static_cast<uint8_t>((rec_id_ * 31u + i) & 0xFF);
      }
      files_[rec_id_] = {sec, std::move(blob)};
    }
    if (out_sec) *out_sec = sec;
    if (out_size) *out_size = size;
    return true;
  }
  bool recording() const override { return recording_; }
  uint32_t record_elapsed_s() const override {
    if (!recording_) return 0;
    return static_cast<uint32_t>((s_clock.now_ms() - rec_start_ms_) / 1000);
  }
  uint8_t record_level() const override {
    // 見た目用に擬似レベルを揺らす。
    return static_cast<uint8_t>(30 + (s_clock.now_ms() / 300) % 40);
  }
  bool play_begin(uint32_t memo_id, uint8_t) override {
    if (playing_ || recording_ || !files_.count(memo_id)) return false;
    playing_ = true;
    play_id_ = memo_id;
    play_end_ms_ = s_clock.now_ms() + files_[memo_id].first * 1000;
    return true;
  }
  void play_stop() override { playing_ = false; }
  bool playing() const override {
    return playing_ && s_clock.now_ms() < play_end_ms_;
  }
  void beep(watch::BeepKind, uint8_t) override {}
  bool memo_audio_size(uint32_t memo_id, uint32_t* out) override {
    auto it = files_.find(memo_id);
    if (it == files_.end()) return false;
    if (out) *out = static_cast<uint32_t>(it->second.second.size());
    return true;
  }
  bool memo_audio_read(uint32_t memo_id, uint32_t off, void* buf,
                       size_t* len) override {
    auto it = files_.find(memo_id);
    if (it == files_.end() || !buf || !len) return false;
    const auto& b = it->second.second;
    if (off >= b.size()) {
      *len = 0;
      return true;
    }
    const size_t n = std::min(*len, b.size() - static_cast<size_t>(off));
    std::memcpy(buf, b.data() + off, n);
    *len = n;
    return true;
  }
  void memo_audio_erase(uint32_t memo_id) override { files_.erase(memo_id); }

 private:
  bool recording_ = false;
  bool playing_ = false;
  uint32_t rec_id_ = 0;
  uint32_t play_id_ = 0;
  int64_t rec_start_ms_ = 0;
  int64_t play_end_ms_ = 0;
  std::map<uint32_t, std::pair<uint32_t, std::vector<uint8_t>>> files_;
};

static SimAudio s_audio;

SimClock& clock() { return s_clock; }
watch::KeyValueStore& kv() { return s_kv; }
watch::Log& log() { return s_log; }
watch::AudioPort& audio() { return s_audio; }

}  // namespace sim
