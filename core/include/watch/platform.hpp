// platform.hpp — ハード依存を切り離すためのインターフェース。
// core/ は ESP-IDF / LVGL / FreeRTOS のヘッダを include しない。
// 実装は firmware/ 側が提供する (RTC, NVS, ESP_LOG など)。
#pragma once

#include <cstddef>
#include <cstdint>

namespace watch {

// 時計。now_ms は monotonic (起動からの経過)、epoch_s は壁時計 (Unix秒)。
class Clock {
 public:
  virtual ~Clock() = default;
  // monotonic ミリ秒。sleep 中も狂わない実装を期待する (esp_timer 相当)。
  virtual int64_t now_ms() = 0;
  // 壁時計 (Unix epoch 秒)。RTC から読む。
  virtual int64_t epoch_s() = 0;
  // 壁時計を合わせる (phone の time.set)。未対応なら false。
  virtual bool set_epoch_s(int64_t epoch_s) = 0;
};

// Key-Value ストア。キーは短い文字列、値はバイト列。
// 実装は firmware 側 (NVS blob 相当)。core はバックエンドを知らない。
class KeyValueStore {
 public:
  virtual ~KeyValueStore() = default;
  // key が無ければ 0。あれば必要なバイト数を返す。
  virtual size_t size(const char* key) = 0;
  // key から読む。buf_cap 不足なら false を返し out_len に必要量を入れる。
  virtual bool get(const char* key, void* buf, size_t buf_cap, size_t* out_len) = 0;
  // key に書く。len==0 は値の消去と同等でもよい。
  virtual bool set(const char* key, const void* data, size_t len) = 0;
  virtual bool erase(const char* key) = 0;
};

// ロガー。実装は ESP_LOG や printf。
enum class LogLevel : uint8_t { Debug, Info, Warn, Error };

class Log {
 public:
  virtual ~Log() = default;
  virtual void write(LogLevel level, const char* tag, const char* message) = 0;
};

// ログ出力先の差し替え (未設定なら何もしない)。
void log_set(Log* log);
Log* log_get();
void log_write(LogLevel level, const char* tag, const char* message);

}  // namespace watch
