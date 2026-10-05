// 起動時診断 (docs/plan.md R 章 Phase 0 の実機確認用)
#pragma once

namespace diag {

// chip 情報 / flash サイズ / heap(internal, PSRAM) / リセット理由をログ出力
void print_system_info();

// BSP の I2C バスをスキャンして応答のあったアドレスをログ出力
void i2c_scan();

}  // namespace diag
