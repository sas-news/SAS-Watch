// face_data.hpp — 文字盤が欲しい補助データの弱いインターフェース。
//   歩数・通知数・次のアラームは別の Feature が並行実装中のため、
//   文字盤はこの getter だけを呼ぶ。未配線の項目は -1/-2 で返り、
//   文字盤側は "--" 表示や非表示にフォールバックする。
//   配線は sim/firmware 側が起動時に set_hooks() で行う。
#pragma once

#include <cstdint>

namespace ui::face_data {

// 1回分の取得結果。負値 = データなし (文字盤は "--" 等に倒す)。
struct Snapshot {
  int32_t steps = -1;          // 今日の歩数。-1 = 不明
  int32_t steps_goal = 0;      // 歩数目標。0/負 = 目標なし
  int32_t notifications = -1;  // 未読通知数。-1 = 不明
  int32_t next_alarm_min = -1; // 次のアラーム (当日 min-of-day)。-1 = なし
};

// データ供給側のフック。nullptr = その項目は未実装。
struct Hooks {
  int32_t (*steps)() = nullptr;
  int32_t (*steps_goal)() = nullptr;
  int32_t (*notifications)() = nullptr;
  int32_t (*next_alarm_min)() = nullptr;
};

// 起動時に1回。h=nullptr で全項目を未実装に戻す。
void set_hooks(const Hooks* h);

// 現在値をまとめて取る。電池/BLE は port / Event 経由なので含めない。
Snapshot get();

}  // namespace ui::face_data
