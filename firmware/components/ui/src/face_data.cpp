// face_data.cpp — 文字盤補助データの受け口実装。
//   フックが無い項目は Snapshot の既定値 (-1/0) のまま返る。
#include "ui/face_data.hpp"

namespace ui::face_data {
namespace {
Hooks s_hooks{};
}

void set_hooks(const Hooks* h) { s_hooks = h ? *h : Hooks{}; }

Snapshot get() {
  Snapshot s;
  if (s_hooks.steps) s.steps = s_hooks.steps();
  if (s_hooks.steps_goal) s.steps_goal = s_hooks.steps_goal();
  if (s_hooks.notifications) s.notifications = s_hooks.notifications();
  if (s_hooks.next_alarm_min) s.next_alarm_min = s_hooks.next_alarm_min();
  return s;
}

}  // namespace ui::face_data
