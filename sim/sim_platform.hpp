// sim_platform.hpp — sim_platform.cpp の実体を外部へ出す窓口。
#pragma once

#include "watch/muse.hpp"
#include "watch/platform.hpp"

namespace sim {

class SimClock : public watch::Clock {
 public:
  int64_t now_ms() override { return now_ms_; }
  int64_t epoch_s() override { return epoch_s_; }
  bool set_epoch_s(int64_t e) override {
    epoch_s_ = e;
    return true;
  }
  void advance_ms(int64_t ms) {
    now_ms_ += ms;
    epoch_s_ = base_epoch_ + now_ms_ / 1000;
  }
  void set_base_epoch(int64_t e) {
    base_epoch_ = e;
    epoch_s_ = e;
  }

 private:
  int64_t now_ms_ = 0;
  int64_t epoch_s_ = 0;
  int64_t base_epoch_ = 0;
};

SimClock& clock();
watch::KeyValueStore& kv();
watch::Log& log();
// 録音/再生のメモリ実装 (メモ画面のスナップショット用)。
watch::AudioPort& audio();

// OTA 画面のスナップショット用に進捗状態を差し替える。
void set_ota_debug(int stage, int pct, const char* msg, const char* version);

// Muse (Phase 2 ⑤) のメモリ実装。スナップショットごとに状態/返答を差し替える。
// reply を渡すと世代番号が進み、feature が取り込み直す。
watch::MusePort& muse();
void muse_set(bool available, watch::MuseState state, const char* error,
              const char* reply);
// 音声入力 (15 秒) のサンプル状態を差し替える。
void muse_set_voice(watch::MuseVoiceState state, uint32_t elapsed_s,
                    const char* error);

}  // namespace sim
