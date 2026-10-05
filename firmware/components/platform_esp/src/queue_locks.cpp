#include "platform_esp/platform_esp.hpp"

#include "freertos/FreeRTOS.h"

// ActionQueue のロックフック: portMUX スピンロック。
// push はボタンコールバック (タスク)・LVGLタスク・将来のISR から呼ばれるので
// portENTER_CRITICAL_SAFE (in_isr を見て ISR 版へ自動切替) を使う。

namespace platform_esp {

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void lock(void*) { portENTER_CRITICAL_SAFE(&s_mux); }
static void unlock(void*) { portEXIT_CRITICAL_SAFE(&s_mux); }

void install_queue_locks(watch::ActionQueue& q) {
  q.set_lock_hooks(lock, unlock, nullptr);
}

}  // namespace platform_esp
