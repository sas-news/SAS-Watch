// platform_esp — core/include/watch/platform.hpp の ESP-IDF 実装。
//   Clock          : monotonic = esp_timer、壁時計 = time() + RTC (board層)
//   KeyValueStore  : NVS blob ("watch" namespace)
//   Log            : ESP_LOGx へ流す
//   ActionQueue    : portMUX スピンロックのロックフック (ISR/タスク両対応)
#pragma once

#include "watch/platform.hpp"
#include "watch/runtime.hpp"

namespace platform_esp {

// シングルトン実体。app_main の初期化順で作って watch_app に渡す。
// 実体は静的に確保される (起動時のみの動的確保ルールに合わせて static 確保)。

// monotonic = esp_timer_get_time() (deep sleep で止まるのは core 側が
// epoch 変換で処理済み — timer.cpp save/restore 参照)。
// epoch_s = time(nullptr)。RTC 反映は board::rtc::apply_to_system_time が
// 起動時に済んでいる前提。set_epoch_s は settimeofday + RTC 書き戻し。
class EspClock : public watch::Clock {
 public:
  int64_t now_ms() override;
  int64_t epoch_s() override;
  bool set_epoch_s(int64_t epoch_s) override;
};

// NVS blob 1キー1値。namespace "watch"。open は init() で1回だけ。
class NvsKv : public watch::KeyValueStore {
 public:
  bool init();  // false = オープン失敗 (以降の操作は全部失敗を返す)
  size_t size(const char* key) override;
  bool get(const char* key, void* buf, size_t buf_cap, size_t* out_len) override;
  bool set(const char* key, const void* data, size_t len) override;
  bool erase(const char* key) override;
};

// ESP_LOGx へ流すロガー。
class EspLog : public watch::Log {
 public:
  void write(watch::LogLevel level, const char* tag,
             const char* message) override;
};

// ActionQueue のロックフックを portMUX スピンロックで差し込む。
// タスク・ISR 両方から安全 (portENTER_CRITICAL_SAFE)。
void install_queue_locks(watch::ActionQueue& q);

// core のログを esp_log に出す。app_main が最初に呼ぶ。
void install_log();

}  // namespace platform_esp
