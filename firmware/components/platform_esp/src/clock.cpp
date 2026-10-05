#include "platform_esp/platform_esp.hpp"

#include <sys/time.h>

#include "esp_timer.h"
#include "esp_log.h"

#include "board/board.hpp"

namespace platform_esp {

int64_t EspClock::now_ms() { return esp_timer_get_time() / 1000; }

int64_t EspClock::epoch_s() { return static_cast<int64_t>(time(nullptr)); }

bool EspClock::set_epoch_s(int64_t epoch_s) {
  if (epoch_s <= 0) return false;
  const struct timeval tv = {.tv_sec = static_cast<time_t>(epoch_s),
                             .tv_usec = 0};
  settimeofday(&tv, nullptr);
  // RTC にも書き戻す — 電源断でリセットされても時刻を保つ。
  // RTC が初期化できていなければシステム時刻だけ合わせて true。
  board::rtc::set_epoch(static_cast<time_t>(epoch_s));
  return true;
}

}  // namespace platform_esp
