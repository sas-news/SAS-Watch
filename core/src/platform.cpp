#include "watch/platform.hpp"

namespace watch {

namespace {
Log* g_log = nullptr;
}

void log_set(Log* log) { g_log = log; }
Log* log_get() { return g_log; }
void log_write(LogLevel level, const char* tag, const char* message) {
  if (g_log) g_log->write(level, tag, message);
}

}  // namespace watch
