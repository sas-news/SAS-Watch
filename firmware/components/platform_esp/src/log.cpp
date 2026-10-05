#include "platform_esp/platform_esp.hpp"

#include "esp_log.h"

namespace platform_esp {

void EspLog::write(watch::LogLevel level, const char* tag,
                   const char* message) {
  switch (level) {
    case watch::LogLevel::Debug: ESP_LOGD(tag, "%s", message); break;
    case watch::LogLevel::Info:  ESP_LOGI(tag, "%s", message); break;
    case watch::LogLevel::Warn:  ESP_LOGW(tag, "%s", message); break;
    case watch::LogLevel::Error: ESP_LOGE(tag, "%s", message); break;
  }
}

static EspLog s_log;

void install_log() { watch::log_set(&s_log); }

}  // namespace platform_esp
