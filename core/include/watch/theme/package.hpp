// theme/package.hpp — テーマパッケージ (stored zip) 読み取りと
// LVGL バイナリ画像 (.bin) ヘッダ検査。docs/theme-format.md 参照。
#pragma once

#include <cstddef>
#include <cstdint>

namespace watch {

constexpr int kThemePackageMaxEntries = 16;

struct ThemePackageEntry {
  char name[48];       // [a-z0-9._-] のみ・ディレクトリ不可 (検査済み)
  uint32_t offset;     // local file header の位置
  uint32_t size;       // 展開後サイズ (= 格納サイズ。stored のみ受理)
  uint32_t crc32;      // 格納データの CRC32
};

// zip[n] のセントラルディレクトリを読む。
// 成功: エントリ数 (out に最大 cap 個)。失敗: -1
//   (EOCD 無し / 圧縮エントリあり / 危険な名前 / 範囲外)。
int theme_package_list(const uint8_t* zip, size_t n, ThemePackageEntry* out,
                       int cap);

// エントリのデータ位置 (local header + 名前 + extra の先)。範囲外なら nullptr。
const uint8_t* theme_package_data(const uint8_t* zip, size_t n,
                                  const ThemePackageEntry* e);

// name のエントリを探す (theme_package_list の結果から線形検索)。
const ThemePackageEntry* theme_package_find(const ThemePackageEntry* list,
                                            int count, const char* name);

// エントリ名ルール ([a-z0-9._-] / ディレクトリ不可 / ".." 不可)。
// ストリーミング展開側 (firmware) も同じルールを使う。
bool theme_package_name_ok(const char* s, size_t n);

// EOCD (0x06054b50) を末尾から探す。見つかればその位置。
// コメント付き zip にも対応するため末尾から遡る。
const uint8_t* theme_package_eocd(const uint8_t* zip, size_t n);

// CRC32 (IEEE 802.3, zip と同じ)。
uint32_t theme_crc32(const uint8_t* p, size_t n);

// .bin 画像ヘッダ (lv_image_header_t 互換) の検査。
// magic 0x19 / cf ∈ {0x12 RGB565, 0x14 RGB565A8} / stride==w*2 /
// data_size == w*h*(2 or 3) / w*h <= max*max。
bool theme_image_check(const uint8_t* bin, size_t n, uint16_t max_w,
                       uint16_t max_h);
// 画像の幅・高さを取る (theme_image_check 成功後に呼ぶ)。
void theme_image_size(const uint8_t* bin, uint16_t* w, uint16_t* h);

}  // namespace watch
