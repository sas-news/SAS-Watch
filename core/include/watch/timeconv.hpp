// timeconv.hpp — 時刻変換の純粋関数。
// mktime/gmtime (TZ 環境変数に依存する) を使わず UTC 固定で変換する。
// RTC (PCF85063A) のレジスタ格納形式とのパック/アンパックもここ。
// ハード非依存なので host テスト可能。
#pragma once

#include <cstdint>

namespace watch {
namespace timeconv {

// UTC 日時。wday は 0=日 .. 6=土 (1970-01-01 は木曜=4)。
struct DateTime {
  int32_t year = 1970;  // 4桁の西暦
  uint8_t month = 1;    // 1-12
  uint8_t day = 1;      // 1-31
  uint8_t wday = 4;     // 0-6
  uint8_t hour = 0;     // 0-23 (24h 表記)
  uint8_t min = 0;      // 0-59
  uint8_t sec = 0;      // 0-59
};

// フィールドが RTC からの読み出し値として信用できるか。
// BCD 値域に加えて、電源喪失 (POR) 直後の全クリア値 (year=1970 相当) を
// 弾くため year >= 2000 を要求する。
bool datetime_valid(const DateTime& dt);

// UTC epoch 秒 -> 日時 (UTC)。常に成功する。
DateTime epoch_to_civil(int64_t epoch_s);

// 日時 -> UTC epoch 秒 (UTC)。値域外フィールド (month=0 等) は -1。
int64_t civil_to_epoch(const DateTime& dt);

// BCD (binary-coded decimal) 変換。
uint8_t dec_to_bcd(uint8_t v);  // 0-99 -> BCD バイト (範囲外は下位2桁のみ)
uint8_t bcd_to_dec(uint8_t v);  // BCD バイト -> 0-99 (ニブル不正はそのまま計算)
// BCD として正しいか (各ニブル <= 9)。
bool bcd_valid(uint8_t v);

// ---- PCF85063A レジスタ形式 (docs/board.md の RTC) ------------------------
// レジスタ配置 (データシート Table 5):
//   0x04 Seconds  (bit7 = OS: 発振停止フラグ)
//   0x05 Minutes  (bit7 未使用)
//   0x06 Hours    (bit7-6 未使用; 24h モードでは bits5:0 = 0-23)
//   0x07 Days     (bits5:0)
//   0x08 Weekdays (bits2:0)
//   0x09 Months   (bits4:0)
//   0x0A Years    (全ビット; 実年 = 1970 + 値)
//   0x0B-0x0F     アラーム (bit7 AEN: 1=そのフィールド無効)
// Hours は 24h モード前提 (CTRL1 bit1 12_24 = 0)。
// 12h モードだと bit5 が AM/PM になるので、このコードでは 12h には対応しない。
namespace pcf85063 {

// regs は 0x04 からの連続 7 バイト。
// 解釈できない値 (BCD 不正/値域外) なら false。OS フラグ (regs[0] bit7) は
// out_os に返す (時刻そのものは読めるが「信用できない」印)。
bool unpack_time(const uint8_t regs[7], DateTime* out, bool* out_os);
// out[7] = 0x04-0x0A に書くバイト列 (OS ビットは 0 で書く)。
void pack_time(const DateTime& dt, uint8_t out[7]);
// アラームレジスタ (0x0B-0x0F) 用: sec/min/hour 一致のみ有効化
// (day/weekday は AEN=1 で無効)。out[5] = 書き込みバイト列。
void pack_alarm(const DateTime& dt, uint8_t out[5]);

constexpr uint8_t kSecondOsFlag = 0x80;   // Seconds レジスタ bit7
constexpr uint8_t kCtrl1_12_24 = 0x02;    // Control_1 bit1: 0=24h, 1=12h
constexpr uint8_t kAlarmFieldOff = 0x80;  // AEN ビット

}  // namespace pcf85063
}  // namespace timeconv
}  // namespace watch
