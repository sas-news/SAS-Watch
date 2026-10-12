#include "platform_esp/platform_esp.hpp"

#include <sys/time.h>

#include "esp_timer.h"
#include "esp_log.h"

#include "board/board.hpp"

namespace platform_esp {

namespace {
const char* kTag = "platform_esp.clock";
}

int64_t EspClock::now_ms() { return esp_timer_get_time() / 1000; }

int64_t EspClock::epoch_s() { return static_cast<int64_t>(time(nullptr)); }

bool EspClock::set_epoch_s(int64_t epoch_s) {
  if (epoch_s <= 0) return false;
  const struct timeval tv = {.tv_sec = static_cast<time_t>(epoch_s),
                             .tv_usec = 0};
  settimeofday(&tv, nullptr);
  // RTC にも書き戻す — 電源断でリセットされても時刻を保つ。
  // 失敗してもシステム時刻は合っているので true のまま返すが、書き込みが
  // 効いていないと次回起動で古い RTC に戻るので必ずログに残す。
  const esp_err_t rc = board::rtc::set_epoch(static_cast<time_t>(epoch_s));
  if (rc != ESP_OK) {
    ESP_LOGW(kTag, "rtc write failed: %s", esp_err_to_name(rc));
  }
  return true;
}

}  // namespace platform_esp
