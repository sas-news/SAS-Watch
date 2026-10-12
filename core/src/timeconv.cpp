// timeconv.cpp — UTC 固定の時刻変換 + PCF85063A レジスタ形式。
// mktime/gmtime/setenv(TZ) を使わないので TZ の取り違えで hour が
// 潰れる心配がない。変換は Howard Hinnant の days_from_civil 系。
#include "watch/timeconv.hpp"

namespace watch {
namespace timeconv {

namespace {

constexpr int64_t kSecPerDay = 86400;

// y/m/d (m:1-12, d:1-31) -> 1970-01-01 からの日数。
// 値域外 (month=0 等) は呼ばないこと。
int64_t days_from_civil(int32_t y, int m, int d) {
  y -= m <= 2;
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const uint32_t yoe = static_cast<uint32_t>(y - era * 400);
  const uint32_t doy =
      (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + static_cast<uint32_t>(d - 1);
  const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int64_t>(era) * 146097 + doe - 719468;
}

// 1970-01-01 からの日数 -> y/m/d。
void civil_from_days(int64_t z, int32_t* y, int* m, int* d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const uint32_t doe = static_cast<uint32_t>(z - era * 146097);
  const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int32_t yy = static_cast<int32_t>(yoe) + static_cast<int32_t>(era) * 400;
  const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint32_t mp = (5 * doy + 2) / 153;
  *d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  *m = static_cast<int>(mp + (mp < 10 ? 3 : -9));
  *y = yy + (*m <= 2);
}

}  // namespace

bool datetime_valid(const DateTime& dt) {
  // year >= 2000: RTC の POR 直後 (year reg = 0 -> 1970) を弾く。
  return dt.year >= 2000 && dt.year <= 2099 && dt.month >= 1 &&
         dt.month <= 12 && dt.day >= 1 && dt.day <= 31 && dt.hour <= 23 &&
         dt.min <= 59 && dt.sec <= 59;
}

DateTime epoch_to_civil(int64_t epoch_s) {
  DateTime dt;
  const int64_t days = epoch_s / kSecPerDay;
  int64_t rem = epoch_s % kSecPerDay;
  if (rem < 0) rem += kSecPerDay;  // epoch < 0 でも時刻部は正に正規化
  int32_t y;
  int m, d;
  civil_from_days(days, &y, &m, &d);
  dt.year = y;
  dt.month = static_cast<uint8_t>(m);
  dt.day = static_cast<uint8_t>(d);
  dt.wday = static_cast<uint8_t>((days + 4) % 7 < 0 ? (days + 4) % 7 + 7
                                                  : (days + 4) % 7);
  dt.hour = static_cast<uint8_t>(rem / 3600);
  dt.min = static_cast<uint8_t>((rem % 3600) / 60);
  dt.sec = static_cast<uint8_t>(rem % 60);
  return dt;
}

int64_t civil_to_epoch(const DateTime& dt) {
  if (dt.month < 1 || dt.month > 12 || dt.day < 1 || dt.day > 31 ||
      dt.hour > 23 || dt.min > 59 || dt.sec > 59) {
    return -1;
  }
  const int64_t days =
      days_from_civil(dt.year, dt.month, dt.day);
  return days * kSecPerDay + static_cast<int64_t>(dt.hour) * 3600 +
         static_cast<int64_t>(dt.min) * 60 + dt.sec;
}

uint8_t dec_to_bcd(uint8_t v) { return static_cast<uint8_t>((v / 10) * 16 + v % 10); }

uint8_t bcd_to_dec(uint8_t v) { return static_cast<uint8_t>((v / 16) * 10 + v % 16); }

bool bcd_valid(uint8_t v) { return (v & 0x0F) <= 9 && (v >> 4) <= 9; }

namespace pcf85063 {

namespace {

// mask 後の値が BCD かつ上限以内なら out に書いて true。
bool field(uint8_t raw, uint8_t mask, uint8_t max, uint8_t* out) {
  const uint8_t v = raw & mask;
  if (!bcd_valid(v)) return false;
  const uint8_t d = bcd_to_dec(v);
  if (d > max) return false;
  *out = d;
  return true;
}

}  // namespace

bool unpack_time(const uint8_t regs[7], DateTime* out, bool* out_os) {
  if (!regs || !out) return false;
  if (out_os) *out_os = (regs[0] & kSecondOsFlag) != 0;
  DateTime dt;
  if (!field(regs[0], 0x7F, 59, &dt.sec)) return false;
  if (!field(regs[1], 0x7F, 59, &dt.min)) return false;
  // Hours: 24h モードの bits5:0 = 0-23。bit5 を含むマスクは AMPM ビットでは
  // なく 20-23 時のため (12h モードではこの解釈は成立しない)。
  if (!field(regs[2], 0x3F, 23, &dt.hour)) return false;
  if (!field(regs[3], 0x3F, 31, &dt.day)) return false;
  if (!field(regs[4], 0x07, 6, &dt.wday)) return false;
  if (!field(regs[5], 0x1F, 12, &dt.month)) return false;
  if (dt.day < 1 || dt.month < 1) return false;
  if (!bcd_valid(regs[6])) return false;
  dt.year = 1970 + bcd_to_dec(regs[6]);
  *out = dt;
  return true;
}

void pack_time(const DateTime& dt, uint8_t out[7]) {
  if (!out) return;
  out[0] = dec_to_bcd(dt.sec);                    // OS ビットは書かない
  out[1] = dec_to_bcd(dt.min);
  out[2] = dec_to_bcd(dt.hour);
  out[3] = dec_to_bcd(dt.day);
  out[4] = dec_to_bcd(dt.wday);
  out[5] = dec_to_bcd(dt.month);
  const int32_t y = dt.year - 1970;
  out[6] = dec_to_bcd(static_cast<uint8_t>(y >= 0 ? (y > 99 ? 99 : y) : 0));
}

void pack_alarm(const DateTime& dt, uint8_t out[5]) {
  if (!out) return;
  // bit7 AEN=0 でそのフィールドの一致を有効化。day/weekday は無効化。
  out[0] = static_cast<uint8_t>(dec_to_bcd(dt.sec) & ~kAlarmFieldOff);
  out[1] = static_cast<uint8_t>(dec_to_bcd(dt.min) & ~kAlarmFieldOff);
  out[2] = static_cast<uint8_t>(dec_to_bcd(dt.hour) & ~kAlarmFieldOff);
  out[3] = kAlarmFieldOff;  // day alarm 無効
  out[4] = kAlarmFieldOff;  // weekday alarm 無効
}

}  // namespace pcf85063
}  // namespace timeconv
}  // namespace watch
