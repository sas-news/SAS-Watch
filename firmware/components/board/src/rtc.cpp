#include "board/board.hpp"
#include "pins.hpp"

#include <sys/time.h>

#include "esp_log.h"
#include "esp_check.h"
#include "sdkconfig.h"
#include "bsp/esp-bsp.h"
#include "pcf85063a.h"
#include "watch/timeconv.hpp"

namespace board::rtc {

static const char* TAG = "board.rtc";

static pcf85063a_dev_t s_rtc;
static bool s_ready = false;

// mktime/gmtime は使わない。TZ 環境変数に依存する変換 (app_main の JST-9 と
// 干渉し得る) をやめ、core の timeconv (UTC 固定の純粋関数) で行う。
using watch::timeconv::DateTime;

static esp_err_t ensure_24h_mode()
{
    // 12h モードだと Hours レジスタの bit5 が AM/PM になり、マスク 0x3F での
    // 読み出し (24h 前提) が壊れる。pcf85063a_init は CTRL1=0x01 (24h) を
    // 書くが、読み戻して保険をかける。// TODO(hw): 実機で確認
    uint8_t ctrl1 = 0;
    ESP_RETURN_ON_ERROR(
        pcf85063a_read_register(&s_rtc, PCF85063A_RTC_CTRL_1_ADDR, &ctrl1, 1),
        TAG, "rtc ctrl1 read failed");
    if (ctrl1 & watch::timeconv::pcf85063::kCtrl1_12_24) {
        ESP_LOGW(TAG, "RTC in 12h mode; forcing 24h");
        uint8_t buf[2] = {PCF85063A_RTC_CTRL_1_ADDR,
                          static_cast<uint8_t>(
                              ctrl1 & ~watch::timeconv::pcf85063::kCtrl1_12_24)};
        ESP_RETURN_ON_ERROR(pcf85063a_write_register(&s_rtc, buf, 2), TAG,
                            "rtc ctrl1 write failed");
    }
    return ESP_OK;
}

static esp_err_t read_datetime(DateTime* out, bool* out_os)
{
    uint8_t regs[7] = {};
    ESP_RETURN_ON_ERROR(
        pcf85063a_read_register(&s_rtc, PCF85063A_RTC_SECOND_ADDR, regs, 7),
        TAG, "rtc read failed");
    // BCD 不正・値域外 (month=0 等) の値は「読めたが信用できない」ので弾く。
    if (!watch::timeconv::pcf85063::unpack_time(regs, out, out_os)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t init()
{
    esp_err_t ret = pcf85063a_init(&s_rtc, bsp_i2c_get_handle(), pins::kI2cAddrRtc);
    if (ret != ESP_OK) {
        return ret;
    }
    s_ready = true;
    // 起動時に RTC をシステム時刻へ反映 (plan.md R 章 Phase 2)
    ret = ensure_24h_mode();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "RTC 24h check failed (%s) // TODO(hw): 実機で確認",
                 esp_err_to_name(ret));
    }
    ret = apply_to_system_time();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "RTC read/apply failed (%s) // TODO(hw): 実機で確認", esp_err_to_name(ret));
    }
    return ESP_OK;
}

esp_err_t apply_to_system_time()
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    DateTime dt;
    bool os = false;
    ESP_RETURN_ON_ERROR(read_datetime(&dt, &os), TAG, "bad rtc value");
    // OS フラグ = 発振が止まった履歴 (電源喪失など)。値は読めても信用
    // できないので、適用はするが警告を出す。// TODO(hw): 実機で確認
    if (os) {
        ESP_LOGW(TAG, "RTC oscillator-stop flag set; time may be stale");
    }
    if (!watch::timeconv::datetime_valid(dt)) {
        ESP_LOGW(TAG, "RTC implausible date %04d-%02u-%02u; not applied",
                 dt.year, dt.month, dt.day);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const int64_t epoch = watch::timeconv::civil_to_epoch(dt);
    if (epoch < 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const struct timeval tv = { .tv_sec = static_cast<time_t>(epoch), .tv_usec = 0 };
    settimeofday(&tv, nullptr);

    ESP_LOGI(TAG, "RTC %04u-%02u-%02u %02u:%02u:%02u UTC -> system time",
             static_cast<unsigned>(dt.year), dt.month, dt.day, dt.hour, dt.min, dt.sec);
    return ESP_OK;
}

esp_err_t get_epoch(time_t* out)
{
    if (!s_ready || out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    DateTime dt;
    ESP_RETURN_ON_ERROR(read_datetime(&dt, nullptr), TAG, "bad rtc value");
    const int64_t epoch = watch::timeconv::civil_to_epoch(dt);
    if (epoch < 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *out = static_cast<time_t>(epoch);
    return ESP_OK;
}

esp_err_t set_epoch(time_t t)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    const DateTime dt = watch::timeconv::epoch_to_civil(static_cast<int64_t>(t));
    uint8_t regs[7];
    watch::timeconv::pcf85063::pack_time(dt, regs);
    uint8_t buf[8] = {PCF85063A_RTC_SECOND_ADDR,
                      regs[0], regs[1], regs[2], regs[3],
                      regs[4], regs[5], regs[6]};
    ESP_RETURN_ON_ERROR(pcf85063a_write_register(&s_rtc, buf, 8), TAG, "rtc write failed");
    return ESP_OK;
}

esp_err_t set_alarm_epoch(time_t t)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    const DateTime dt = watch::timeconv::epoch_to_civil(static_cast<int64_t>(t));
    // PCF85063A のアラームは sec/min/hour に一致 (day/weekday は無効)。
    // 次回発火時刻を都度入れ直す運用なのでこれで十分。
    uint8_t packed[5];
    watch::timeconv::pcf85063::pack_alarm(dt, packed);
    uint8_t buf[6] = {PCF85063A_RTC_SECOND_ALARM,
                      packed[0], packed[1], packed[2], packed[3], packed[4]};
    ESP_RETURN_ON_ERROR(pcf85063a_write_register(&s_rtc, buf, 6), TAG, "rtc alarm write failed");
    // AIE=1 + AF clear (発火後に INT が LOW のままだと light sleep が即起きる
    // ので、再設定・解除のたびに呼んでクリアする) // TODO(hw): 実機で確認
    return pcf85063a_enable_alarm(&s_rtc);
}

esp_err_t clear_alarm()
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    // AIE=0 + AF=0。driver に disable 関数が無いので CTRL2 を直接書く
    // TODO(hw): 実機で確認 (CTRL2 の他ビットを保持すべきか)
    uint8_t buf[2] = { PCF85063A_RTC_CTRL_2_ADDR, 0x00 };
    return pcf85063a_write_register(&s_rtc, buf, 2);
}

}  // namespace board::rtc
