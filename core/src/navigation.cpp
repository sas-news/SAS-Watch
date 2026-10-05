#include "watch/navigation.hpp"

namespace watch {

void Navigator::push(Route r) {
  if (r == Route::None) return;
  if (depth_ >= kMaxDepth) {
    // 最大4段。超えたら Home 以外で一番古いものを捨てる
    // (stack_[0]=Home はトップなので常に残す)。
    for (uint8_t i = 1; i < depth_ - 1; ++i) stack_[i] = stack_[i + 1];
    stack_[depth_ - 1] = r;
  } else {
    stack_[depth_++] = r;
  }
  emit_changed();
}

void Navigator::replace(Route r) {
  if (r == Route::None) return;
  stack_[depth_ - 1] = r;
  emit_changed();
}

void Navigator::pop() {
  if (modal_ != Route::None) {
    dismiss_modal();
    return;
  }
  if (depth_ > 1) {
    --depth_;
    emit_changed();
  }
}

void Navigator::pop_to_home() {
  if (modal_ != Route::None) modal_ = Route::None;
  if (depth_ > 1) {
    depth_ = 1;
    emit_changed();
  }
}

void Navigator::present_modal(Route r) {
  if (r == Route::None || modal_ == r) return;
  modal_ = r;
  emit_changed();
}

void Navigator::dismiss_modal() {
  if (modal_ == Route::None) return;
  modal_ = Route::None;
  emit_changed();
}

Route Navigator::at(uint8_t i) const {
  if (i >= depth_) return Route::None;
  return stack_[depth_ - 1 - i];
}

bool Navigator::handle_action(const Action& a) {
  switch (a.type) {
    case ActionType::Navigate:
      push(static_cast<Route>(a.arg0));
      return true;
    case ActionType::Home:
      pop_to_home();
      return true;
    case ActionType::Back:
      if (modal_ != Route::None || depth_ > 1) {
        pop();
        return true;
      }
      return false;  // Home 末端: 呼び出し側が画面OFFに使う
    default:
      return false;
  }
}

void Navigator::emit_changed() {
  if (bus_) {
    bus_->publish({EventType::RouteChanged, static_cast<uint32_t>(current())});
  }
}

}  // namespace watch
