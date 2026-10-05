// input.cpp — board の物理ボタン → InputMapper → Action → queue。
// board 側のコールバックはボタンタスクのコンテキストで来る。
#include "board/board.hpp"
#include "watch/input_mapper.hpp"
#include "watch_app/watch_app.hpp"

namespace watch_app {

static watch::InputMapper* s_mapper = nullptr;

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
  if (s_mapper->translate(be, &a)) {
    a.source = watch::ActionSource::Button;
    push_action(a);
  }
}

void input_init(watch::InputMapper* mapper) {
  s_mapper = mapper;
  board::buttons::set_callback(on_button);
}

}  // namespace watch_app
