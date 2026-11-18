// テスト用フェイク。PC 専用なので STL を使ってよい。
#pragma once

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "watch/event_bus.hpp"
#include "watch/muse.hpp"
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

// 録音/再生をメモリで真似する AudioPort フェイク。
// 音声メモの「ファイル」は id と秒数から決定論的に作るので、
// sha256 などをテスト側で再現できる。
class FakeAudio : public AudioPort {
 public:
  // 1 秒あたりの保存バイト数 (16kHz ADPCM ≈ 8KB/s 想定)。
  static constexpr uint32_t kBytesPerSec = 8000;
  static constexpr uint32_t kFileHeader = 16;

  void set_clock(Clock* c) { clock_ = c; }

  bool fail_record_begin = false;
  bool fail_play_begin = false;
  // 録音経過をテスト側で直接決める (0 なら clock から計算)。
  uint32_t rec_elapsed_s = 0;
  // 再生中に止まらないでほしいとき true (自然終了させない)。
  bool play_forever = true;
  // 録音ストリーム (dictation) 用のテストノブ。
  bool stream_ok = true;           // false = この個体ではストリーム不可
  bool fail_stream_enable = false;
  int stream_enable_calls = 0;
  uint32_t stream_dropped = 0;

  struct BeepRec { BeepKind kind; uint8_t vol; };
  std::vector<BeepRec> beeps;
  uint32_t last_play_id = 0;
  uint32_t last_play_vol = 0;
  int record_begin_calls = 0;
  int record_end_calls = 0;
  int erase_calls = 0;

  bool record_begin(uint32_t memo_id, uint32_t) override {
    ++record_begin_calls;
    if (fail_record_begin || recording_ || playing_) return false;
    recording_ = true;
    rec_id_ = memo_id;
    rec_start_ms_ = clock_ ? clock_->now_ms() : 0;
    rec_elapsed_s = 0;
    return true;
  }

  bool record_end(bool commit, uint32_t* out_sec,
                  uint32_t* out_size) override {
    if (!recording_) return false;
    ++record_end_calls;
    const uint32_t sec = record_elapsed_s();
    recording_ = false;
    const uint32_t size = kFileHeader + sec * kBytesPerSec;
    if (commit) {
      std::vector<uint8_t> blob(size, 0);
      blob[0] = 'A'; blob[1] = 'D'; blob[2] = 'P'; blob[3] = '1';
      for (size_t i = kFileHeader; i < size; ++i) {
        blob[i] = static_cast<uint8_t>((rec_id_ * 31u + i) & 0xFF);
      }
      files_[rec_id_] = {sec, std::move(blob)};
    } else {
      files_.erase(rec_id_);
    }
    if (out_sec) *out_sec = sec;
    if (out_size) *out_size = size;
    return true;
  }

  bool recording() const override { return recording_; }
  uint32_t record_elapsed_s() const override {
    if (rec_elapsed_s > 0) return rec_elapsed_s;
    if (!clock_ || !recording_) return 0;
    return static_cast<uint32_t>((clock_->now_ms() - rec_start_ms_) / 1000);
  }
  uint8_t record_level() const override { return 42; }

  // ---- 録音ストリーム (dictation) ----
  // 実機のリングバッファを真似る。テストは stream_feed() でデータを積み、
  // offset が欠落で飛ぶことを確かめられる。
  bool can_record_stream() const override { return stream_ok; }
  bool record_stream_enable(bool enable) override {
    ++stream_enable_calls;
    if (enable) {
      if (fail_stream_enable) return false;
      stream_enabled_ = true;
      stream_total_ = 0;
      stream_avail_ = 0;
      return true;
    }
    stream_enabled_ = false;
    stream_total_ = 0;
    stream_avail_ = 0;
    return true;
  }
  bool record_stream_read(void* buf, size_t* len_inout) override {
    if (!len_inout) return false;
    if (!stream_enabled_ || !buf) {
      *len_inout = 0;
      return false;
    }
    const size_t n = std::min(*len_inout, static_cast<size_t>(stream_avail_));
    std::memset(buf, 0x5A, n);
    stream_avail_ -= static_cast<uint32_t>(n);
    *len_inout = n;
    return true;
  }
  uint32_t record_stream_offset() const override {
    return stream_total_ - stream_avail_;
  }
  uint32_t record_stream_dropped() const override { return stream_dropped; }

  // テスト用: 録音ストリームに n バイト積む / 古い n バイトを捨てる。
  void stream_feed(uint32_t n) {
    stream_total_ += n;
    stream_avail_ += n;
  }
  void stream_drop_oldest(uint32_t n) {
    const uint32_t d = std::min(n, stream_avail_);
    stream_avail_ -= d;
    stream_dropped += d;
  }

  bool play_begin(uint32_t memo_id, uint8_t volume) override {
    if (fail_play_begin || playing_ || recording_) return false;
    if (!files_.count(memo_id)) return false;
    last_play_id = memo_id;
    last_play_vol = volume;
    playing_ = true;
    play_end_ms_ = clock_ ? clock_->now_ms() +
                                static_cast<int64_t>(files_[memo_id].first) * 1000
                          : INT64_MAX;
    return true;
  }
  void play_stop() override { playing_ = false; }
  bool playing() const override {
    if (!playing_) return false;
    if (play_forever) return true;
    return clock_ && clock_->now_ms() < play_end_ms_;
  }
  void beep(BeepKind kind, uint8_t volume) override {
    beeps.push_back({kind, volume});
  }

  bool memo_audio_size(uint32_t memo_id, uint32_t* out_size) override {
    auto it = files_.find(memo_id);
    if (it == files_.end()) return false;
    if (out_size) *out_size = static_cast<uint32_t>(it->second.second.size());
    return true;
  }
  bool memo_audio_read(uint32_t memo_id, uint32_t offset, void* buf,
                       size_t* len_inout) override {
    auto it = files_.find(memo_id);
    if (it == files_.end() || !buf || !len_inout) return false;
    const auto& blob = it->second.second;
    if (offset >= blob.size()) {
      *len_inout = 0;
      return true;
    }
    const size_t got =
        std::min(*len_inout, blob.size() - static_cast<size_t>(offset));
    std::memcpy(buf, blob.data() + offset, got);
    *len_inout = got;
    return true;
  }
  void memo_audio_erase(uint32_t memo_id) override {
    ++erase_calls;
    files_.erase(memo_id);
    if (playing_ && last_play_id == memo_id) playing_ = false;
  }

  // 録音工程を飛ばして音声メモを差し込む (テストの時短用)。
  void put_audio(uint32_t id, uint32_t sec) {
    const uint32_t size = kFileHeader + sec * kBytesPerSec;
    std::vector<uint8_t> blob(size, 0);
    blob[0] = 'A'; blob[1] = 'D'; blob[2] = 'P'; blob[3] = '1';
    for (size_t i = kFileHeader; i < size; ++i) {
      blob[i] = static_cast<uint8_t>((id * 31u + i) & 0xFF);
    }
    files_[id] = {sec, std::move(blob)};
  }

  void reset() {
    *this = FakeAudio{};
    files_.clear();
    beeps.clear();
  }

 private:
  Clock* clock_ = nullptr;
  bool recording_ = false;
  bool playing_ = false;
  uint32_t rec_id_ = 0;
  int64_t rec_start_ms_ = 0;
  int64_t play_end_ms_ = 0;
  bool stream_enabled_ = false;
  uint32_t stream_total_ = 0;  // 積んだ累計 (offset = total - avail)
  uint32_t stream_avail_ = 0;  // 未読バイト数
  // id → {秒数, バイト列}
  std::map<uint32_t, std::pair<uint32_t, std::vector<uint8_t>>> files_;
};

// Muse セッションのフェイク。状態・返答・エラーをテスト側から差し替える。
class FakeMuse : public MusePort {
 public:
  bool available_ = true;
  int start_calls = 0;
  int stop_calls = 0;
  int send_calls = 0;
  std::string last_sent;

  bool available() const override { return available_; }
  MuseState state() override { return state_; }
  const char* last_error() override { return error_; }
  uint32_t reply_generation() override { return generation_; }
  size_t last_reply(char* out, size_t cap) override {
    if (!out || cap == 0) return 0;
    size_t n = reply_.size();
    if (n > cap - 1) n = cap - 1;
    std::memcpy(out, reply_.data(), n);
    out[n] = '\0';
    return n;
  }
  bool start() override {
    ++start_calls;
    state_ = MuseState::Starting;
    return true;
  }
  void stop() override {
    ++stop_calls;
    state_ = MuseState::Off;
  }
  bool send_text(const char* text) override {
    ++send_calls;
    last_sent = text ? text : "";
    return true;
  }

  // ---- 音声入力 (15 秒の音声ノート) ----
  MuseVoiceState voice_state() override { return voice_; }
  bool voice_start(uint32_t max_sec) override {
    ++voice_start_calls;
    if (voice_ == MuseVoiceState::Recording) return false;
    voice_max_sec = max_sec;
    voice_ = MuseVoiceState::Recording;
    voice_elapsed = 0;
    voice_error_[0] = '\0';
    return true;
  }
  void voice_stop() override {
    ++voice_stop_calls;
    if (voice_ == MuseVoiceState::Recording) voice_ = MuseVoiceState::Sending;
  }
  uint32_t voice_elapsed_s() override { return voice_elapsed; }
  const char* voice_error() override { return voice_error_; }
  void voice_pump() override { ++voice_pump_calls; }

  // ---- テスト用の操作 ----
  void set_state(MuseState st) { state_ = st; }
  void set_error(const char* msg) {
    std::snprintf(error_, sizeof(error_), "%s", msg ? msg : "");
  }
  void set_reply(const char* text) {
    reply_ = text ? text : "";
    ++generation_;
  }
  void set_voice(MuseVoiceState st, uint32_t elapsed, const char* err) {
    voice_ = st;
    voice_elapsed = elapsed;
    std::snprintf(voice_error_, sizeof(voice_error_), "%s", err ? err : "");
  }

  // 音声入力のテストノブ / 呼び出し回数。
  uint32_t voice_max_sec = 0;
  uint32_t voice_elapsed = 0;
  int voice_start_calls = 0;
  int voice_stop_calls = 0;
  int voice_pump_calls = 0;

 private:
  MuseState state_ = MuseState::Off;
  MuseVoiceState voice_ = MuseVoiceState::Idle;
  char voice_error_[96] = {};
  char error_[128] = {};
  std::string reply_;
  uint32_t generation_ = 0;
};

}  // namespace test
}  // namespace watch
