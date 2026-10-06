// theme/zipfile.hpp — ファイル上の stored-zip を読む最小リーダ。
// package.hpp は zip がメモリ上にある前提; こちらは読み取りコールバック経由
// でファイルをランダムアクセスする (firmware: littlefs, sim: stdio)。
// zip 自体は展開せずそのまま置く形 (docs/theme-format.md)。
#pragma once

#include <cstdint>

#include "watch/theme/package.hpp"

namespace watch {

// コールバック型の zip リーダ。at(off, dst, n) で n バイト読む。
struct ThemeZipSrc {
  void* user;
  bool (*at)(void* user, uint32_t off, uint8_t* dst, uint32_t n);
  uint32_t size;  // ファイル全体のバイト数
};

// セントラルディレクトリを走査してエントリ表を作る。
// 成功: エントリ数 (out に最大 cap 個)。失敗: -1。
int theme_zipfile_list(const ThemeZipSrc& z, ThemePackageEntry* out, int cap,
                       uint8_t* tail_buf, uint32_t tail_cap);

// e のデータ位置 (local header + 名前 + extra の先) を返す。失敗: false。
// データは src.at(off, ...) で読む。
bool theme_zipfile_data_offset(const ThemeZipSrc& z,
                               const ThemePackageEntry& e, uint32_t* off);

}  // namespace watch
