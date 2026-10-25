// input.cpp — board の物理ボタン → InputMapper → Action → queue。
// board 側のコールバックはボタンタスクのコンテキストで来る。
#include <atomic>
#include <cstring>

#include "board/board.hpp"
#include "esp_log.h"
#include "watch/input_mapper.hpp"
#include "watch_app/watch_app.hpp"

#include "internal.hpp"

namespace watch_app {

static const char* TAG = "watch_app.input";

static watch::InputMapper* s_mapper = nullptr;

// ボタンタスクから読むので、mapper の表を直接見ずに atomic にキャッシュする
// (mapper の更新は core_lock 下で行われ、同期は input_sync_double_enabled)。
static std::atomic<bool> s_dbl_enabled[2] = {};

static bool double_click_enabled(board::Button b) {
  return s_dbl_enabled[static_cast<size_t>(b)].load(std::memory_order_relaxed);
}

static void on_button(board::Button b, board::ButtonEvent ev) {
  if (!s_mapper) return;
  watch::ButtonEvent be{};
  be.button =
      b == board::Button::Boot ? watch::PhysicalButton::Boot
                               : watch::PhysicalButton::Pwr;
  switch (ev) {
    case board::ButtonEvent::ShortPress: be.press = watch::PressType::Short; break;
    case board::ButtonEvent::LongPress: be.press = watch::PressType::Long; break;
    case board::ButtonEvent::DoubleClick: be.press = watch::PressType::Double; break;
    default: return;
  }
  watch::Action a;
  // 消灯中は「起こすだけ」。割り当てが none でも必ず起こす (core の
  // dispatch は Action が届いて初めて kick するので、none だとボタンでは
  // 画面が点かない穴があった)。core 側も同じ規則で二重に守る。
  const watch::PowerState ps = power().state();
  if (ps == watch::PowerState::ScreenOff ||
      ps == watch::PowerState::DeepSleepCandidate) {
    a.type = watch::ActionType::Wake;
    a.source = watch::ActionSource::Button;
    push_action(a);
    return;
  }
  if (s_mapper->translate(be, &a)) {
    a.source = watch::ActionSource::Button;
    push_action(a);
  } else {
    ESP_LOGI(TAG, "button %d press %d unmapped", static_cast<int>(be.button),
             static_cast<int>(be.press));
  }
}

// mapper の Double 割り当て (「none」以外なら 2回押し有効) を
// s_dbl_enabled に反映する。core_lock 下 (mapper と同じ排他) で呼ぶこと。
void input_sync_double_enabled() {
  if (!s_mapper) return;
  const watch::PhysicalButton bs[2] = {watch::PhysicalButton::Boot,
                                       watch::PhysicalButton::Pwr};
  for (int i = 0; i < 2; ++i) {
    const char* n = s_mapper->action_name(bs[i], watch::PressType::Double);
    s_dbl_enabled[i].store(n != nullptr && std::strcmp(n, "none") != 0,
                           std::memory_order_relaxed);
  }
}

void input_init(watch::InputMapper* mapper) {
  s_mapper = mapper;
  input_sync_double_enabled();
  board::buttons::set_double_click_predicate(double_click_enabled);
  board::buttons::set_callback(on_button);
}

}  // namespace watch_app
