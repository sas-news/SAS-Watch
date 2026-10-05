// navigation.hpp — 画面スタック (最大4段) + モーダル。
// plan.md G章。「トップは Home だけ。Quick/Dev は呼び出す画面。」
#pragma once

#include <cstdint>

#include "watch/action.hpp"
#include "watch/event_bus.hpp"

namespace watch {

enum class Route : uint8_t {
  None = 0,
  Home,
  Quick,
  Notifications,
  Timer,
  Stopwatch,
  Counter,
  Memo,
  More,
  Settings,
  About,
  Media,
  Dev,
  Agent,
  PowerMenu,
  Confirm,  // 承認モーダル用
};

class Navigator {
 public:
  static constexpr uint8_t kMaxDepth = 4;

  // bus に RouteChanged を出したいときだけ渡す (nullptr 可)。
  explicit Navigator(EventBus* bus = nullptr) : bus_(bus) { stack_[0] = Route::Home; }

  void push(Route r);
  void replace(Route r);
  void pop();
  void pop_to_home();
  void present_modal(Route r);
  void dismiss_modal();

  Route current() const { return stack_[depth_ - 1]; }
  Route modal() const { return modal_; }
  bool has_modal() const { return modal_ != Route::None; }
  uint8_t depth() const { return depth_; }
  // 上から順にスタックを覗く。i=0 が現在。範囲外は Route::None。
  Route at(uint8_t i) const;

  // Navigate/Back/Home/PowerMenu 系の Action を処理する。
  // 処理したら true。Back で Home 末端なら false (呼び出し側が画面OFFに使う)。
  bool handle_action(const Action& a);

 private:
  void emit_changed();

  EventBus* bus_;
  Route stack_[kMaxDepth] = {};
  uint8_t depth_ = 1;
  Route modal_ = Route::None;
};

}  // namespace watch
