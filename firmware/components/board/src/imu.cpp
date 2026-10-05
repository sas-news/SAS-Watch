#include "board/board.hpp"
#include "pins.hpp"

#include "esp_log.h"
#include "esp_check.h"
#include "bsp/esp-bsp.h"

#ifdef M_PI
#undef M_PI
#endif
#include "qmi8658.h"

// 公式サンプル 04_Immersive_block の初期化手順 + plan.md Q 章の wake-on-motion。

namespace board::imu {

static const char* TAG = "board.imu";

static qmi8658_dev_t s_imu;
static bool s_ready = false;

// 通常時の加速度計設定。常時動かすので low-power ODR にする
// (歩数/腕上げには ~20Hz で十分)。TODO(hw): 実機で検出精度を確認、
// 取りこぼしが多ければ QMI8658_ACCEL_ODR_125HZ へ。
static void apply_normal_config()
{
    qmi8658_set_accel_range(&s_imu, QMI8658_ACCEL_RANGE_4G);
    qmi8658_set_accel_odr(&s_imu, QMI8658_ACCEL_ODR_LOWPOWER_21HZ);
    qmi8658_enable_sensors(&s_imu, QMI8658_ENABLE_ACCEL);
}

esp_err_t init()
{
    esp_err_t ret = qmi8658_init(&s_imu, bsp_i2c_get_handle(), pins::kI2cAddrImu);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t who = 0;
    qmi8658_get_who_am_i(&s_imu, &who);

    // 時計用途: 加速度計だけ有効化 (ジャイロは使うときに開く)
    apply_normal_config();

    s_ready = true;
    ESP_LOGI(TAG, "QMI8658 ok (who_am_i=0x%02X)", who);
    return ESP_OK;
}

esp_err_t read_accel_mg(float* x, float* y, float* z)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    // qmi8658_read_accel は既定 (accel_unit_mps2=false) で mg を返す
    return qmi8658_read_accel(&s_imu, x, y, z);
}

esp_err_t arm_wake_on_motion()
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    // threshold は plan.md Q 章の例値。INT1(GPIO21)の極性は board.md【要実機】
    // TODO(hw): 実機で閾値と極性を確認
    return qmi8658_enable_wake_on_motion(&s_imu, 0x40);
}

esp_err_t disarm_wake_on_motion()
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    // disable はセンサーを止めて設定も 2G/LP のまま残すので、
    // ポーリング用の通常設定に戻す。
    esp_err_t ret = qmi8658_disable_wake_on_motion(&s_imu);
    if (ret != ESP_OK) {
        return ret;
    }
    apply_normal_config();
    return ESP_OK;
}

}  // namespace board::imu
