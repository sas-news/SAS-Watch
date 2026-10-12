// timeconv (BCD / epoch<->日時 / PCF85063A レジスタ形式) のテスト。
// firmware/components/board/src/rtc.cpp がこの変換を使う。
#include "watch/timeconv.hpp"

#include <gtest/gtest.h>

using watch::timeconv::DateTime;
namespace tc = watch::timeconv;

TEST(TimeConv, BcdRoundTrip) {
  EXPECT_EQ(tc::dec_to_bcd(0), 0x00);
  EXPECT_EQ(tc::dec_to_bcd(9), 0x09);
  EXPECT_EQ(tc::dec_to_bcd(23), 0x23);
  EXPECT_EQ(tc::dec_to_bcd(59), 0x59);
  EXPECT_EQ(tc::dec_to_bcd(99), 0x99);
  for (int v = 0; v <= 99; ++v) {
    EXPECT_EQ(tc::bcd_to_dec(tc::dec_to_bcd(static_cast<uint8_t>(v))), v);
  }
  // bcd_valid: ニブル >9 は不正
  EXPECT_TRUE(tc::bcd_valid(0x59));
  EXPECT_FALSE(tc::bcd_valid(0x5A));
  EXPECT_FALSE(tc::bcd_valid(0xA5));
}

TEST(TimeConv, EpochToCivil) {
  // epoch 0 = 1970-01-01 00:00:00 UTC (木曜=4)
  const DateTime e0 = tc::epoch_to_civil(0);
  EXPECT_EQ(e0.year, 1970);
  EXPECT_EQ(e0.month, 1);
  EXPECT_EQ(e0.day, 1);
  EXPECT_EQ(e0.wday, 4);
  EXPECT_EQ(e0.hour, 0);
  EXPECT_EQ(e0.min, 0);
  EXPECT_EQ(e0.sec, 0);

  // 2025-10-05 09:41:00 UTC (test_apps.cpp と同じ値)
  const DateTime e1 = tc::epoch_to_civil(1'759'657'260);
  EXPECT_EQ(e1.year, 2025);
  EXPECT_EQ(e1.month, 10);
  EXPECT_EQ(e1.day, 5);
  EXPECT_EQ(e1.wday, 0);  // 日曜
  EXPECT_EQ(e1.hour, 9);
  EXPECT_EQ(e1.min, 41);
  EXPECT_EQ(e1.sec, 0);

  // +540min (JST) で 18:01 になること — RTC 書き込み側が UTC を守る限り
  // 表示側の tz 加算で正しいローカル時刻になる。
  const DateTime e2 = tc::epoch_to_civil(1'759'657'260 + 540 * 60);
  EXPECT_EQ(e2.hour, 18);
  EXPECT_EQ(e2.min, 41);

  // UTC 深夜帯 (RTC では hour=0 が正しい値。潰れて 0 になるのと区別する)。
  const DateTime e3 = tc::epoch_to_civil(3600 * 0 + 34 * 60);  // 00:34 UTC
  EXPECT_EQ(e3.hour, 0);
  EXPECT_EQ(e3.min, 34);
}

TEST(TimeConv, CivilToEpoch) {
  DateTime dt;
  dt.year = 2025;
  dt.month = 10;
  dt.day = 5;
  dt.hour = 9;
  dt.min = 41;
  dt.sec = 0;
  EXPECT_EQ(tc::civil_to_epoch(dt), 1'759'657'260);

  // 値域外は -1 (mktime のように勝手に正規化しない)
  DateTime bad = dt;
  bad.month = 0;
  EXPECT_EQ(tc::civil_to_epoch(bad), -1);
  bad = dt;
  bad.day = 0;
  EXPECT_EQ(tc::civil_to_epoch(bad), -1);
  bad = dt;
  bad.hour = 24;
  EXPECT_EQ(tc::civil_to_epoch(bad), -1);
  bad = dt;
  bad.sec = 60;
  EXPECT_EQ(tc::civil_to_epoch(bad), -1);
}

TEST(TimeConv, CivilEpochRoundTrip) {
  const int64_t cases[] = {0,          1'759'657'260, 2'000'000'000,
                           59,         3600,           86399,
                           86400,      4'000'000'000,  1'234'567'890};
  for (const int64_t e : cases) {
    const DateTime dt = tc::epoch_to_civil(e);
    // datetime_valid は year>=2000 の「RTC の値として信用できる」判定。
    EXPECT_EQ(tc::datetime_valid(dt), dt.year >= 2000) << "epoch=" << e;
    EXPECT_EQ(tc::civil_to_epoch(dt), e) << "epoch=" << e;
  }
}

TEST(TimeConv, DatetimeValid) {
  DateTime dt = tc::epoch_to_civil(1'759'657'260);
  EXPECT_TRUE(tc::datetime_valid(dt));
  // RTC 電源喪失直後相当 (POR で全クリア -> year 1970) は弾く。
  dt.year = 1970;
  EXPECT_FALSE(tc::datetime_valid(dt));
  dt = tc::epoch_to_civil(1'759'657'260);
  dt.month = 0;
  EXPECT_FALSE(tc::datetime_valid(dt));
  dt.day = 0;
  EXPECT_FALSE(tc::datetime_valid(dt));
  dt = tc::epoch_to_civil(1'759'657'260);
  dt.hour = 24;
  EXPECT_FALSE(tc::datetime_valid(dt));
}

TEST(TimeConv, Pcf85063PackUnpack) {
  const DateTime dt = tc::epoch_to_civil(1'759'657'260);  // 2025-10-05 09:01 UTC
  uint8_t regs[7];
  tc::pcf85063::pack_time(dt, regs);
  // レジスタ配置 (0x04-0x0A): sec,min,hour,day,wday,month,year
  EXPECT_EQ(regs[0], 0x00);  // sec=0, OS ビットは立てない
  EXPECT_EQ(regs[1], 0x41);  // min=41
  EXPECT_EQ(regs[2], 0x09);  // hour=9
  EXPECT_EQ(regs[3], 0x05);  // day=5
  EXPECT_EQ(regs[4], 0x00);  // wday=0 (日)
  EXPECT_EQ(regs[5], 0x10);  // month=10
  EXPECT_EQ(regs[6], 0x55);  // year=2025-1970=55

  DateTime back;
  bool os = false;
  ASSERT_TRUE(tc::pcf85063::unpack_time(regs, &back, &os));
  EXPECT_FALSE(os);
  EXPECT_EQ(back.year, 2025);
  EXPECT_EQ(back.month, 10);
  EXPECT_EQ(back.day, 5);
  EXPECT_EQ(back.wday, 0);
  EXPECT_EQ(back.hour, 9);
  EXPECT_EQ(back.min, 41);
  EXPECT_EQ(back.sec, 0);
}

TEST(TimeConv, Pcf85063UnpackRejectsGarbage) {
  DateTime dt;
  // BCD 不正 (min = 0x5A)
  uint8_t regs[7] = {0x00, 0x5A, 0x09, 0x05, 0x00, 0x10, 0x55};
  EXPECT_FALSE(tc::pcf85063::unpack_time(regs, &dt, nullptr));
  // hour 24h 値域外 (0x24 = 24 は不正。12h モードの AM/PM 混入も弾ける)
  uint8_t regs2[7] = {0x00, 0x41, 0x24, 0x05, 0x00, 0x10, 0x55};
  EXPECT_FALSE(tc::pcf85063::unpack_time(regs2, &dt, nullptr));
  // month=0 / day=0 (POR 直後のクリア値) は弾く
  uint8_t regs3[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  EXPECT_FALSE(tc::pcf85063::unpack_time(regs3, &dt, nullptr));
  // OS フラグ (regs[0] bit7) は読み取れるが時刻値には影響しない
  uint8_t regs4[7] = {0x80, 0x41, 0x09, 0x05, 0x00, 0x10, 0x55};
  bool os = false;
  ASSERT_TRUE(tc::pcf85063::unpack_time(regs4, &dt, &os));
  EXPECT_TRUE(os);
  EXPECT_EQ(dt.sec, 0);
  EXPECT_EQ(dt.hour, 9);
}

TEST(TimeConv, Pcf85063PackAlarm) {
  const DateTime dt = tc::epoch_to_civil(1'759'657'260);
  uint8_t a[5];
  tc::pcf85063::pack_alarm(dt, a);
  // sec/min/hour 一致のみ有効 (AEN=0)。day/weekday は 0x80 で無効。
  EXPECT_EQ(a[0], 0x00);
  EXPECT_EQ(a[1], 0x41);
  EXPECT_EQ(a[2], 0x09);
  EXPECT_EQ(a[3], 0x80);
  EXPECT_EQ(a[4], 0x80);
}
