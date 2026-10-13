#pragma once
// perf.hpp — 画面遷移ごとの初回 render 計測 (roadmap T0-3)。
// swap_screen で画面名を渡し、表示の LV_EVENT_RENDER_START→READY の
// 最初の完了を `perf: screen=<name> render=<ms>` として1行出す。

namespace ui {
void perf_mark_screen(const char* name);
}  // namespace ui
