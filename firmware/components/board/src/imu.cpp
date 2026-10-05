#include "board/board.hpp"
#include "pins.hpp"

#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
static bool s_hw_ped = false;  // ハード歩数計が有効か

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

// --- ハードウェア歩数計 -------------------------------------------------
// QMI8658A DS §11: pedometer パラメータを CAL レジスタ経由で CTRL9 コマンド
// 0x0D ×2回で投入 → CTRL8.bit4 でエンジン有効化 → STEP_CNT(0x5A-5C) 読み出し。
// rev0.8 C datasheet には pedometer 節が無い (rev0.9 C は CTRL8.bit4 と
// STATUS1.bit4 だけ確認可) → 値は A datasheet 準拠の推測。// TODO(hw): 実機で確認

namespace {

// レジスタ (QMI8658A DS Table 22/28/38)
constexpr uint8_t kRegCtrl7    = 0x08;
constexpr uint8_t kRegCtrl8    = 0x09;
constexpr uint8_t kRegCtrl9    = 0x0A;
constexpr uint8_t kRegCal1L    = 0x0B;
constexpr uint8_t kRegCal1H    = 0x0C;
constexpr uint8_t kRegCal2L    = 0x0D;
constexpr uint8_t kRegCal2H    = 0x0E;
constexpr uint8_t kRegCal3L    = 0x0F;
constexpr uint8_t kRegCal3H    = 0x10;
constexpr uint8_t kRegCal4L    = 0x11;
constexpr uint8_t kRegCal4H    = 0x12;
constexpr uint8_t kRegStatusInt = 0x2D;
constexpr uint8_t kRegStepCnt  = 0x5A;  // STEP_CNT_LOW/MIDL/HIGH (3連番)

constexpr uint8_t kCtrl9CmdConfigPedometer = 0x0D;  // CTRL_CMD_CONFIGURE_PEDOMETER
constexpr uint8_t kCtrl9CmdResetPedometer  = 0x0F;  // CTRL_CMD_RESET_PEDOMETER
constexpr uint8_t kCtrl8PedEnable          = 0x10;  // CTRL8.bit4
constexpr uint8_t kStatusIntCmdDone        = 0x80;  // STATUSINT.bit7 (rev0.9)

esp_err_t wreg(uint8_t reg, uint8_t v) {
    return qmi8658_write_register(&s_imu, reg, v);
}

// CTRL9 コマンド実行 + CmdDone 待ち。
// 完了は STATUSINT.bit7 (rev0.9)。rev0.8 では STATUS1.bit0 とあるので
// 両方ポーリングする。// TODO(hw): 実機でどちらが立つか確認
esp_err_t ctrl9_exec(uint8_t cmd)
{
    esp_err_t ret = wreg(kRegCtrl9, cmd);
    if (ret != ESP_OK) return ret;
    for (int i = 0; i < 50; ++i) {  // 最大 ~100ms
        uint8_t si = 0, s1 = 0;
        if (qmi8658_read_register(&s_imu, kRegStatusInt, &si, 1) != ESP_OK) {
            return ESP_FAIL;
        }
        if (si & kStatusIntCmdDone) return ESP_OK;  // 読取でクリア+INT1解除
        if (qmi8658_read_register(&s_imu, 0x2F, &s1, 1) != ESP_OK) {
            return ESP_FAIL;
        }
        if (s1 & 0x01) return ESP_OK;  // rev0.8 流儀 (STATUS1.bit0)
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return ESP_ERR_TIMEOUT;
}

}  // namespace

esp_err_t hw_pedometer_init()
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    // deep sleep 復帰などで既に有効なら採用 (カウントはチップ側で継続)。
    uint8_t ctrl8 = 0;
    esp_err_t ret = qmi8658_read_register(&s_imu, kRegCtrl8, &ctrl8, 1);
    if (ret != ESP_OK) return ret;
    if (ctrl8 & kCtrl8PedEnable) {
        s_hw_ped = true;
        ESP_LOGI(TAG, "pedometer already enabled (deep sleep 継続)");
        return ESP_OK;
    }

    // 設定は accel/gyro 停止中に行うこと (DS §11.2)。
    ret = qmi8658_enable_sensors(&s_imu, QMI8658_DISABLE_ALL);
    if (ret != ESP_OK) goto fail;

    // パラメータ (DS Table 38)。DS 例は ODR=50Hz 前提だが、うちは常時
    // LOWPOWER_21HZ なので時間系の値を 21/50 にスケール。// TODO(hw): 計数精度
    // set1: ped_sample_cnt=21 (~1s), ped_fix_peak2peak=200mg, ped_fix_peak=100mg
    wreg(kRegCal1L, 0x15);  // 21
    wreg(kRegCal1H, 0x00);
    wreg(kRegCal2L, 0xCC);  // 200 (u6.10 → ~195mg)
    wreg(kRegCal2H, 0x00);
    wreg(kRegCal3L, 0x66);  // 100
    wreg(kRegCal3H, 0x00);
    wreg(kRegCal4L, 0x00);
    wreg(kRegCal4H, 0x01);  // 1st command 印
    ret = ctrl9_exec(kCtrl9CmdConfigPedometer);
    if (ret != ESP_OK) goto fail;

    // set2: ped_time_up=84 (~4s), ped_time_low=8 (~0.38s), ped_cnt_entry=10,
    //       ped_fix_precision=0, ped_sig_count=4
    wreg(kRegCal1L, 0x54);  // 84
    wreg(kRegCal1H, 0x00);
    wreg(kRegCal2L, 0x08);  // 8
    wreg(kRegCal2H, 0x0A);  // 10 steps entry
    wreg(kRegCal3L, 0x00);
    wreg(kRegCal3H, 0x04);  // 4 steps 毎に STEP_CNT 更新
    wreg(kRegCal4L, 0x00);
    wreg(kRegCal4H, 0x02);  // 2nd command 印
    ret = ctrl9_exec(kCtrl9CmdConfigPedometer);
    if (ret != ESP_OK) goto fail;

    // CTRL8.bit4=1 で Pedometer engine 有効化 + カウンタをクリア。
    ret = wreg(kRegCtrl8, ctrl8 | kCtrl8PedEnable);
    if (ret != ESP_OK) goto fail;
    ret = ctrl9_exec(kCtrl9CmdResetPedometer);
    if (ret != ESP_OK) goto fail;

    s_hw_ped = true;
    apply_normal_config();
    ESP_LOGI(TAG, "hw pedometer on (odr=21Hz 前提のパラメータ)");
    return ESP_OK;

fail:
    ESP_LOGW(TAG, "hw pedometer init failed (%s) -> sw fallback",
             esp_err_to_name(ret));
    apply_normal_config();
    return ret;
}

bool hw_pedometer_active() { return s_hw_ped; }

esp_err_t hw_pedometer_steps(uint32_t* out)
{
    if (!s_ready || !out) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t b[3] = {0};
    esp_err_t ret = qmi8658_read_register(&s_imu, kRegStepCnt, b, 3);
    if (ret != ESP_OK) return ret;
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16);
    return ESP_OK;
}

}  // namespace board::imu
