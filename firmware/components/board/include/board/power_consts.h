// power_consts.h — 電源まわりの定数。pmic.cpp のレジスタ設定と
// UI の案内文を同じ値から出すための唯一のソース (ESP非依存、sim でも使える)。
#pragma once

namespace board {

// PWR ボタン長押しで AXP2101 がハード電源 OFF する秒数。
// AXP2101 の設定値 (4/6/8/10s のいずれか) と一致させること。
constexpr int kPowerOffHoldSeconds = 6;  // TODO(hw): 実機で確認

}  // namespace board
