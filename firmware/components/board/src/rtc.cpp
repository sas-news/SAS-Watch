#include "board/board.hpp"
#include "pins.hpp"

#include <sys/time.h>

#include "esp_log.h"
#include "esp_check.h"
#include "sdkconfig.h"
#include "bsp/esp-bsp.h"
#include "pcf85063a.h"

namespace board::rtc {

static const char* TAG = "board.rtc";

static pcf85063a_dev_t s_rtc;
static bool s_ready = false;
static bool s_tz_set = false;

static void ensure_utc_tz()
{
    if (!s_tz_set) {
        // RTC との変換は常に UTC で行う (表示用の TZ は app 側で別途設定)
        setenv("TZ", "UTC0", 1);
        tzset();
        s_tz_set = true;
    }
}

static esp_err_t datetime_to_epoch(const pcf85063a_datetime_t* dt, time_t* out)
{
    struct tm t = {};
    t.tm_year = (int)dt->year - 1900;
    t.tm_mon = (int)dt->month - 1;
    t.tm_mday = dt->day;
    t.tm_hour = dt->hour;
    t.tm_min = dt->min;
    t.tm_sec = dt->sec;
    ensure_utc_tz();
    const time_t epoch = mktime(&t);
    if (epoch == (time_t)-1) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *out = epoch;
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
    pcf85063a_datetime_t dt;
    ESP_RETURN_ON_ERROR(pcf85063a_get_time_date(&s_rtc, &dt), TAG, "rtc read failed");

    time_t epoch;
    ESP_RETURN_ON_ERROR(datetime_to_epoch(&dt, &epoch), TAG, "bad rtc value");

    const struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, nullptr);

    ESP_LOGI(TAG, "RTC %04u-%02u-%02u %02u:%02u:%02u UTC -> system time",
             dt.year, dt.month, dt.day, dt.hour, dt.min, dt.sec);
    return ESP_OK;
}

esp_err_t get_epoch(time_t* out)
{
    if (!s_ready || out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    pcf85063a_datetime_t dt;
    ESP_RETURN_ON_ERROR(pcf85063a_get_time_date(&s_rtc, &dt), TAG, "rtc read failed");
    return datetime_to_epoch(&dt, out);
}

esp_err_t set_epoch(time_t t)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);

    const pcf85063a_datetime_t dt = {
        .year = static_cast<uint16_t>(tm_utc.tm_year + 1900),
        .month = static_cast<uint8_t>(tm_utc.tm_mon + 1),
        .day = static_cast<uint8_t>(tm_utc.tm_mday),
        .dotw = static_cast<uint8_t>(tm_utc.tm_wday),
        .hour = static_cast<uint8_t>(tm_utc.tm_hour),
        .min = static_cast<uint8_t>(tm_utc.tm_min),
        .sec = static_cast<uint8_t>(tm_utc.tm_sec),
    };
    ESP_RETURN_ON_ERROR(pcf85063a_set_time_date(&s_rtc, dt), TAG, "rtc write failed");
    return ESP_OK;
}

esp_err_t set_alarm_epoch(time_t t)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    // PCF85063A のアラームは sec/min/hour に一致 (day/weekday は無効)。
    // 次回発火時刻を都度入れ直す運用なのでこれで十分。
    const pcf85063a_datetime_t dt = {
        .year = static_cast<uint16_t>(tm_utc.tm_year + 1900),
        .month = static_cast<uint8_t>(tm_utc.tm_mon + 1),
        .day = static_cast<uint8_t>(tm_utc.tm_mday),
        .dotw = static_cast<uint8_t>(tm_utc.tm_wday),
        .hour = static_cast<uint8_t>(tm_utc.tm_hour),
        .min = static_cast<uint8_t>(tm_utc.tm_min),
        .sec = static_cast<uint8_t>(tm_utc.tm_sec),
    };
    ESP_RETURN_ON_ERROR(pcf85063a_set_alarm(&s_rtc, dt), TAG, "rtc alarm write failed");
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
