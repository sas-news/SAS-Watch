// theme_res.hpp — テーマ資産 (.bin/.png 画像, .bin フォント) の
// 遅延ロードと LVGL オブジェクト化。すべて LVGL タスク内で呼ぶ。
//
// 画像は「呼び出し側が用意した領域」にデコードする設計:
//   - グローバル資産 (icons/mascot/face用) → port::theme_asset_load のアリーナ
//   - 画面背景 → port::theme_screen_block() のブロック
// PNG は lodepng で一度だけ RGB565A8 に変換してしまうので、
// LVGL の画像デコーダキャッシュは使わない (常駐メモリを食わないため)。
#pragma once

#include <cstdint>

#include "lvgl.h"

namespace theme_res {

// エントリ名 file を領域 dst[cap] に読み込み、dsc を組み立てる。
// .bin (12B ヘッダ + RGB565/RGB565A8) はそのまま、.png は lodepng で
// RGB565A8 に変換する。戻り値: dst に書いたバイト数 (0=失敗)。
// max_w/max_h はサイズ上限 (超過は失敗)。
uint32_t img_decode(const char* id, const char* file, uint8_t* dst,
                    uint32_t cap, uint16_t max_w, uint16_t max_h,
                    lv_image_dsc_t* dsc);

// アリーナに確保して読み込む (dsc は静的プールから供給)。失敗 nullptr。
const lv_image_dsc_t* img_load(const char* id, const char* file,
                               uint16_t max_w, uint16_t max_h);

// .bin フォント (lv_binfont) を読み込む。フォントデータは
// アリーナ上に保持され、lv_binfont_create_from_buffer が参照する。
// 失敗 nullptr。生成したフォントは reset() でまとめて解放。
const lv_font_t* font_load(const char* id, const char* file);

// ロード済みフォントと画像 dsc プールを全て解放する
// (port::theme_assets_reset の直前に必須)。
void reset();

}  // namespace theme_res
