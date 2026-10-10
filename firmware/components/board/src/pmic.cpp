#include "board/board.hpp"
#include "board/power_consts.h"
#include "pins.hpp"

#include <stdio.h>
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

// battery_percent() の EMA 平滑用 (permille 保持)。初回実測値で初期化する
static int s_ema_permille = 0;
static bool s_ema_initialized = false;

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
    // reg 0x24 (Vsys power-off 閾値) は既定値の確認のみで変更はしない。
    // T-Watch 系の知見では最小値 2.6V 付近が安全。実機で既定値を見てから判断
    // TODO(hw): 実機で出力された値を確認してから変更を検討
    ESP_LOGI(TAG, "vsys power-off threshold=%umV (reg 0x24)",
             (unsigned)s_pmu.getSysPowerDownVoltage());
    return ESP_OK;
}

int battery_percent()
{
    if (!s_ready || !s_pmu.isBatteryConnect()) {
        return -1;
    }
    const int raw = s_pmu.getBatteryPercent();
    // 電圧推定で値が跳ねるので EMA (alpha=1/4) で平滑化。初回は実測値を採用
    if (!s_ema_initialized) {
        s_ema_permille = raw * 1000;
        s_ema_initialized = true;
    } else {
        s_ema_permille += (raw * 1000 - s_ema_permille) / 4;
    }
    return (s_ema_permille + 500) / 1000;
}

bool is_charging()
{
    return s_ready && s_pmu.isCharging();
}

bool is_vbus()
{
    return s_ready && s_pmu.isVbusIn();
}

ChargeStatus charge_status()
{
    if (!s_ready) {
        return ChargeStatus::Unknown;
    }
    if (!s_pmu.isBatteryConnect() || s_pmu.isDischarge()) {
        return ChargeStatus::NotCharging;
    }
    switch (s_pmu.getChargerStatus()) {
    case XPOWERS_AXP2101_CHG_TRI_STATE:   // トリクルもプリチャージの一種として扱う
    case XPOWERS_AXP2101_CHG_PRE_STATE:
        return ChargeStatus::PreCharging;
    case XPOWERS_AXP2101_CHG_CC_STATE:
        return ChargeStatus::ConstantCurrent;
    case XPOWERS_AXP2101_CHG_CV_STATE:
        return ChargeStatus::ConstantVoltage;
    case XPOWERS_AXP2101_CHG_DONE_STATE:
        return ChargeStatus::FullCharged;
    case XPOWERS_AXP2101_CHG_STOP_STATE:
        return ChargeStatus::Stopped;
    default:
        return ChargeStatus::Unknown;
    }
}

bool pmic_status(PmicStatus* out)
{
    if (!s_ready || out == nullptr) {
        return false;
    }
    out->percent = battery_percent();
    out->charging = s_pmu.isCharging();
    out->status = charge_status();
    out->vbus_good = s_pmu.isVbusGood();
    out->discharging = s_pmu.isDischarge();
    out->batt_v = s_pmu.getBattVoltage() / 1000.0f;
    out->vbus_v = s_pmu.getVbusVoltage() / 1000.0f;
    out->pmic_temp_c = s_pmu.getTemperature();
    return true;
}

namespace {

struct Rail {
    const char* name;
    bool on;
    uint16_t mv;
};

void log_rail_group(const char* label, const Rail* rails, size_t n)
{
    char line[192];
    int pos = snprintf(line, sizeof(line), "rails %s:", label);
    for (size_t i = 0; i < n && pos > 0 && pos < (int)sizeof(line); ++i) {
        pos += snprintf(line + pos, sizeof(line) - (size_t)pos, " %s=%s/%u.%03uV",
                        rails[i].name, rails[i].on ? "on" : "off",
                        rails[i].mv / 1000u, rails[i].mv % 1000u);
    }
    ESP_LOGI(TAG, "%s", line);
}

}  // namespace

void rail_dump()
{
    if (!s_ready) {
        ESP_LOGW(TAG, "pmic not ready; rail dump skipped");
        return;
    }
    const Rail dcdc[] = {
        {"DC1", s_pmu.isEnableDC1(), s_pmu.getDC1Voltage()},
        {"DC2", s_pmu.isEnableDC2(), s_pmu.getDC2Voltage()},
        {"DC3", s_pmu.isEnableDC3(), s_pmu.getDC3Voltage()},
        {"DC4", s_pmu.isEnableDC4(), s_pmu.getDC4Voltage()},
        {"DC5", s_pmu.isEnableDC5(), s_pmu.getDC5Voltage()},
    };
    const Rail aldo[] = {
        {"ALDO1", s_pmu.isEnableALDO1(), s_pmu.getALDO1Voltage()},
        {"ALDO2", s_pmu.isEnableALDO2(), s_pmu.getALDO2Voltage()},
        {"ALDO3", s_pmu.isEnableALDO3(), s_pmu.getALDO3Voltage()},
        {"ALDO4", s_pmu.isEnableALDO4(), s_pmu.getALDO4Voltage()},
    };
    const Rail ldo[] = {
        {"BLDO1", s_pmu.isEnableBLDO1(), s_pmu.getBLDO1Voltage()},
        {"BLDO2", s_pmu.isEnableBLDO2(), s_pmu.getBLDO2Voltage()},
        {"CPUSLDO", s_pmu.isEnableCPUSLDO(), s_pmu.getCPUSLDOVoltage()},
        {"DLDO1", s_pmu.isEnableDLDO1(), s_pmu.getDLDO1Voltage()},
        {"DLDO2", s_pmu.isEnableDLDO2(), s_pmu.getDLDO2Voltage()},
    };
    log_rail_group("DCDC", dcdc, sizeof(dcdc) / sizeof(dcdc[0]));
    log_rail_group("ALDO", aldo, sizeof(aldo) / sizeof(aldo[0]));
    log_rail_group("LDO", ldo, sizeof(ldo) / sizeof(ldo[0]));
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
