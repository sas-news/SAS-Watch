#include "board/board.hpp"
#include "pins.hpp"

#include <cstdio>

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "soc/soc.h"
#include "soc/rtc_io_reg.h"
#include "soc/rtc_cntl_reg.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// docs/board.md / plan.md A-3:
// - deep sleep から復帰できるのは PWR(GPIO10), IMU(GPIO21), timer だけ
//   (GPIO38 タッチ, GPIO39 RTC は RTC GPIO ではないので deep sleep では使えない)
// - BOOT(GPIO0) は RTC GPIO だが strapping pin でもある: deep sleep 起床は
//   チップリセットなので、起床のために押した BOOT の LOW がそのまま
//   ストラップサンプルに残り、UART download mode で起動してしまう
//   (= 復帰不能に見える)。そのため ext0 (BOOT) は deep sleep では張らない。
// - light sleep なら GPIO38 タッチや BOOT でも復帰できる

namespace board::sleep {

static const char* TAG = "board.sleep";

void arm_light_sleep_wake()
{
    // LOW アクティブ: タッチ INT / BOOT ボタン
    gpio_wakeup_enable(pins::kTouchInt, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable(pins::kButtonBoot, GPIO_INTR_LOW_LEVEL);
    // HIGH アクティブ: PWR ボタン
    gpio_wakeup_enable(pins::kButtonPwr, GPIO_INTR_HIGH_LEVEL);
    // LOW アクティブ: RTC INT (アラーム発火)。deep sleep では使えない
    // (RTC GPIO ではない) ので light sleep 専用。
    gpio_wakeup_enable(pins::kRtcInt, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    ESP_LOGI(TAG, "light sleep wake: touch(GPIO38,LOW) boot(GPIO0,LOW) pwr(GPIO10,HIGH) rtc(GPIO39,LOW)");
}

void arm_light_sleep_imu()
{
    // GPIO21 は imu_service が GPIO 割り込み (POSEDGE) で WoM を受けるので
    // gpio_wakeup_enable (LEVEL 型に int 型を書き換える) は呼べない。
    // light sleep が現状未使用のため何もしない。
    // TODO(hw): light sleep 実装時に再検討 (割り込み型の共存 or 排他)
}

void disarm_light_sleep_imu()
{
    gpio_wakeup_disable(pins::kImuInt1);
}

void log_pin_levels(const char* tag)
{
    // S3 には esp_sleep_get_gpio_wakeup_status が無いので、
    // 起床直後 (または入眠直前) の実レベルを出して切り分ける。
    ESP_LOGI(tag, "levels: boot=%d pwr=%d imu=%d touch=%d rtc=%d",
             gpio_get_level(pins::kButtonBoot), gpio_get_level(pins::kButtonPwr),
             gpio_get_level(pins::kImuInt1), gpio_get_level(pins::kTouchInt),
             gpio_get_level(pins::kRtcInt));
}

bool woke_by_imu()
{
    // WoM の INT1 は STATUS1 読取まで保持されるレベル出力（QMI8658C DS §6.1）
    // なので起床時にピンが HIGH なら IMU 起因とみなす。
    // TODO(hw): 実機で確認（短いパルスなら取りこぼす可能性あり）
    return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO &&
           gpio_get_level(pins::kImuInt1) == 1;
}

void enter_deep_sleep(uint64_t wake_after_us, bool imu_wake,
                      int32_t* rtc_level_out, uint32_t* regs_out)
{
    // light sleep 用に貼り付けた歩数同期タイマー (imu_service が set) を
    // deep sleep に持ち込まない。以降は wake_after_us だけが timer になる。
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);

    // arm_light_sleep_wake() の gpio_wakeup_enable は RTC_GPIO_TRIG_EN を
    // 立てる。これは deep sleep にも残り、RTC GPIO (GPIO0/GPIO10) が指定
    // レベルなら入眠直後に起きてしまう (= 復帰不能に見える or ウエイクループ)。
    // GPIO38/39 は非 RTC なので効かないが、GPIO10 (PWR) は ext1 と二重になる
    // ので GPIO 側は全て外して ext1 に一本化する。
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);

    // 入眠時の実レベルを残す (即起床ループの切り分け用)。
    log_pin_levels(TAG);

    // RTC 側レベルを既知ピンと対照して読み、入眠時の実レベルを診断に残す。
    //   g0 (BOOT 解放=HIGH) / g10 (PWR 解放=LOW) / g21 (IMU INT1)
    // この初期化は機能上も必要: rtc_gpio_init が LP IO クロック
    // (io_mux_enable_lp_io_clock) の参照カウントを上げるので、pad10/21 を
    // deinit せず残しておくことで深眠中の RTC パッド入力が保証される。
    uint32_t lvls = 0;
    const gpio_num_t probe[] = {GPIO_NUM_0, GPIO_NUM_10, GPIO_NUM_21};
    for (const gpio_num_t g : probe) {
        rtc_gpio_init(g);
        rtc_gpio_set_direction(g, RTC_GPIO_MODE_INPUT_ONLY);
    }
    lvls |= rtc_gpio_get_level(GPIO_NUM_0);        // bit0: BOOT (expect 1)
    lvls |= rtc_gpio_get_level(GPIO_NUM_10) << 1;  // bit1: PWR  (expect 0)
    lvls |= rtc_gpio_get_level(GPIO_NUM_21) << 2;  // bit2: IMU  (期待1)
    rtc_gpio_pullup_en(GPIO_NUM_21);
    esp_rom_delay_us(50);  // プルアップが効くまで少し待つ
    lvls |= rtc_gpio_get_level(GPIO_NUM_21) << 3;  // bit3: IMU+pullup
    lvls |= gpio_get_level(GPIO_NUM_21) << 4;      // bit4: dig 側の g21
    rtc_gpio_pullup_dis(GPIO_NUM_21);
    ESP_LOGI(TAG, "rtc lvls g0=%d g10=%d g21=%d g21pu=%d dig21=%d",
             (int)(lvls & 1), (int)((lvls >> 1) & 1), (int)((lvls >> 2) & 1),
             (int)((lvls >> 3) & 1), (int)((lvls >> 4) & 1));
    // 生レジスタも診断に残す — pad21 の MUX_SEL/FUN_IE、hold 残留、in_val。
    {
        const uint32_t p21 = REG_READ(RTC_IO_RTC_PAD21_REG);
        const uint32_t p10 = REG_READ(RTC_IO_TOUCH_PAD10_REG);
        const uint32_t hold = REG_READ(RTC_CNTL_PAD_HOLD_REG);
        const uint32_t in = REG_READ(RTC_GPIO_IN_REG);
        ESP_LOGI(TAG,
                 "regs p21=%08lx(mux=%ld ie=%ld slp_sel=%ld slp_ie=%ld "
                 "rue=%ld rde=%ld) p10=%08lx hold=%08lx in=%08lx",
                 (unsigned long)p21, (unsigned long)((p21 >> 19) & 1),
                 (unsigned long)((p21 >> 13) & 1),
                 (unsigned long)((p21 >> 16) & 1),
                 (unsigned long)((p21 >> 15) & 1),
                 (unsigned long)((p21 >> 27) & 1),
                 (unsigned long)((p21 >> 28) & 1), (unsigned long)p10,
                 (unsigned long)hold, (unsigned long)in);
        if (rtc_level_out) {
            *rtc_level_out = (int32_t)lvls;
        }
        if (regs_out) {
            regs_out[0] = p21;
            regs_out[1] = p10;
            regs_out[2] = hold;
            regs_out[3] = in;
        }
    }
    // BOOT(GPIO0) だけデジタルに戻す。pad10/21 は RTC 入力の
    // まま残す — rtc_gpio_deinit は参照カウント式の LP IO クロック
    // (io_mux_enable_lp_io_clock) を最後の1ピンで落とすため、全ピン deinit
    // すると ext0/ext1 prepare (rtcio_hal 直叩き・クロック有効化無し) が
    // クロック OFF の状態でパッドを設定してしまう。
    rtc_gpio_deinit(GPIO_NUM_0);

    // PWR(GPIO10) / IMU INT1(GPIO21) — ext1, どれかが HIGH で復帰。
    // INT1 極性は実機確認済み: WoM イベントで HIGH ラッチ → ANY_HIGH 起床。
    // imu_wake=false (raise_to_wake OFF) の時に INT1 を入れると、未クリアの
    // WoM ラッチや浮いたピンの HIGH で入眠直後に即起床するので外す。
    uint64_t ext1_mask = (1ULL << pins::kButtonPwr);
    if (imu_wake) {
        ext1_mask |= (1ULL << pins::kImuInt1);
    }
    esp_err_t ret =
        esp_sleep_enable_ext1_wakeup(ext1_mask, ESP_EXT1_WAKEUP_ANY_HIGH);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ext1 wake failed: %s", esp_err_to_name(ret));
    }
    if (imu_wake) {
        // ext0 も併用 — 別機構のレベルコンパレータで冗長化 (実機で ext0 起床
        // を確認済み)。両方立てても起床原因はどちらかが報告されるだけで害は無い。
        const esp_err_t r0 = esp_sleep_enable_ext0_wakeup(pins::kImuInt1, 1);
        ESP_LOGI(TAG, "ext0 imu wake: %s", esp_err_to_name(r0));
    }

    if (wake_after_us > 0) {
        esp_sleep_enable_timer_wakeup(wake_after_us);
    }

    ESP_LOGI(TAG, "entering deep sleep (pwr ext1, imu=%s, timer=%s)",
             imu_wake ? "on" : "off", wake_after_us > 0 ? "on" : "off");
    // 入眠直前に WoM ラッチを最終クリア (init_low では STATUS1 読取で
    // INT1 は休息 LOW に戻る)。入眠時の実レベルを残す。
    if (imu_wake) {
        board::imu::wom_clear();
        vTaskDelay(pdMS_TO_TICKS(5));
        ESP_LOGI(TAG, "final int1: dig=%d rtc=%d",
                 (int)gpio_get_level(pins::kImuInt1),
                 (int)rtc_gpio_get_level(pins::kImuInt1));
    }
    // USB-Serial/JTAG は deep sleep で切れるので、直前ログの送出を待つ
    // (100ms では TX FIFO が host に汲まれる前に切れて喪失した — 500ms)。
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_deep_sleep_start();
    // ここには戻らない
    for (;;) {
    }
}

}  // namespace board::sleep
