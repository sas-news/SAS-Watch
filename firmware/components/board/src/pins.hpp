// GPIO 割り当て (docs/board.md の表と一致させること)
#pragma once

#include "driver/gpio.h"

namespace board::pins {

inline constexpr gpio_num_t kButtonBoot = GPIO_NUM_0;    // BOOT ボタン, 押下 LOW,  RTC GPIO
inline constexpr gpio_num_t kButtonPwr  = GPIO_NUM_10;   // PWR ボタン, 押下 HIGH, RTC GPIO
inline constexpr gpio_num_t kImuInt1    = GPIO_NUM_21;   // QMI8658 INT1, 極性 TODO(hw): 実機で確認
inline constexpr gpio_num_t kTouchInt   = GPIO_NUM_38;   // タッチ INT, LOW アクティブ (light sleep wake のみ)
inline constexpr gpio_num_t kRtcInt     = GPIO_NUM_39;   // RTC INT (light sleep wake のみ)
inline constexpr gpio_num_t kMotor      = GPIO_NUM_18;   // モーター駆動 (NPN 経由) // TODO(hw): モーター有無

inline constexpr uint8_t kI2cAddrPmic = 0x34;  // AXP2101
inline constexpr uint8_t kI2cAddrRtc  = 0x51;  // PCF85063ATL
inline constexpr uint8_t kI2cAddrImu  = 0x6B;  // QMI8658C (SA0=GND)

}  // namespace board::pins
