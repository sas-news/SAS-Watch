// sim_platform.hpp — sim_platform.cpp の実体を外部へ出す窓口。
#pragma once

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

}  // namespace sim
