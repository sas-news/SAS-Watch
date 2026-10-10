// 起動時診断 (docs/plan.md R 章 Phase 0 の実機確認用)
#pragma once

namespace diag {

// chip 情報 / flash サイズ / heap(internal, PSRAM) / リセット理由をログ出力
void print_system_info();

// BSP の I2C バスをスキャンして応答のあったアドレスをログ出力
void i2c_scan();

// AXP2101 全レール (DC/ALDO/BLDO/SLDO) の ON/OFF + 電圧をログ出力。
// board::init() 済みであること (呼ぶなら i2c_scan() の後あたり)
void pmic_rail_dump();

}  // namespace diag
