#include "board/board.hpp"
#include "board/power_consts.h"
#include "pins.hpp"

#include <string.h>

#include "esp_log.h"
#include "esp_check.h"
#include "sdkconfig.h"
#include "bsp/esp-bsp.h"

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

// 公式サンプル 01_AXP2101/main/port_axp2101.cpp を BSP の I2C バスに載せた版。
// AXP IRQ 線は ESP32 に未接続 (回路図・BSP PMU_INTERRUPT_PIN=-1) なので
// 状態は I2C ポーリングで読む (docs/plan.md A-3)。

namespace board::pmic {

static const char* TAG = "board.pmic";
static constexpr int kI2cTimeoutMs = 1000;

static XPowersPMU s_pmu;
static i2c_master_dev_handle_t s_dev = nullptr;
static bool s_ready = false;

static int pmu_register_read(uint8_t dev_addr, uint8_t reg_addr, uint8_t* data, uint8_t len)
{
    (void)dev_addr;
    const esp_err_t ret = i2c_master_transmit_receive(s_dev, &reg_addr, 1, data, len, kI2cTimeoutMs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "pmu read reg 0x%02X failed: %s", reg_addr, esp_err_to_name(ret));
        return -1;
    }
    return 0;
}

static int pmu_register_write_byte(uint8_t dev_addr, uint8_t reg_addr, uint8_t* data, uint8_t len)
{
    (void)dev_addr;
    uint8_t buf[16];
    if (len + 1 > sizeof(buf)) {
        return -1;
    }
    buf[0] = reg_addr;
    memcpy(&buf[1], data, len);
    const esp_err_t ret = i2c_master_transmit(s_dev, buf, len + 1, kI2cTimeoutMs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "pmu write reg 0x%02X failed: %s", reg_addr, esp_err_to_name(ret));
        return -1;
    }
    return 0;
}

esp_err_t init()
{
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = pins::kI2cAddrPmic,
        .scl_speed_hz = static_cast<uint32_t>(CONFIG_BSP_I2C_CLK_SPEED_HZ),
        .scl_wait_us = 0,
        .flags = {},
    };
    ESP_RETURN_ON_ERROR(
        i2c_master_bus_add_device(bsp_i2c_get_handle(), &dev_cfg, &s_dev),
        TAG, "add pmic device failed");

    if (!s_pmu.begin(AXP2101_SLAVE_ADDRESS, pmu_register_read, pmu_register_write_byte)) {
        ESP_LOGE(TAG, "AXP2101 begin failed // TODO(hw): 実機で確認");
        return ESP_FAIL;
    }

    // 計測系を有効化。TS pin 計測は切る (電池温度センサ無し → 切らないと充電異常)
    s_pmu.disableTSPinMeasure();
    s_pmu.enableBattDetection();
    s_pmu.enableVbusVoltageMeasure();
    s_pmu.enableBattVoltageMeasure();
    s_pmu.enableSystemVoltageMeasure();
    s_pmu.enableTemperatureMeasure();

    // 充電設定 (公式サンプル値: 定電流 400mA / 終止 25mA / 目標 4.2V)
    s_pmu.setPrechargeCurr(XPOWERS_AXP2101_PRECHARGE_50MA);
    s_pmu.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_400MA);
    s_pmu.setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_25MA);
    s_pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);

    // PWR 長押しでハード電源 OFF (plan.md G 章)。秒数は power_consts.h
    // の board::kPowerOffHoldSeconds から選ぶ (UI の案内文と同じ値)。
    static_assert(board::kPowerOffHoldSeconds == 4 ||
                      board::kPowerOffHoldSeconds == 6 ||
                      board::kPowerOffHoldSeconds == 8 ||
                      board::kPowerOffHoldSeconds == 10,
                  "AXP2101 は 4/6/8/10s しか選べない");
    constexpr xpowers_press_off_time_t kOffTime =
        board::kPowerOffHoldSeconds <= 4  ? XPOWERS_POWEROFF_4S
        : board::kPowerOffHoldSeconds <= 6 ? XPOWERS_POWEROFF_6S
        : board::kPowerOffHoldSeconds <= 8 ? XPOWERS_POWEROFF_8S
                                           : XPOWERS_POWEROFF_10S;
    s_pmu.setPowerKeyPressOffTime(kOffTime);

    // IRQ 線は未接続だがステータスはクリアしておく
    s_pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    s_pmu.clearIrqStatus();

    s_ready = true;
    ESP_LOGI(TAG, "AXP2101 ok: batt=%d%% charging=%d vbus=%d",
             s_pmu.getBatteryPercent(), (int)s_pmu.isCharging(), (int)s_pmu.isVbusIn());
    return ESP_OK;
}

int battery_percent()
{
    return s_ready ? s_pmu.getBatteryPercent() : -1;
}

bool is_charging()
{
    return s_ready && s_pmu.isCharging();
}

bool is_vbus()
{
    return s_ready && s_pmu.isVbusIn();
}

void panel_power(bool on)
{
    if (!s_ready) {
        return;
    }
    // ALDO2 = DSI_PWR_EN (パネル電源)
    if (on) {
        s_pmu.enableALDO2();
    } else {
        s_pmu.disableALDO2();
    }
}

}  // namespace board::pmic
