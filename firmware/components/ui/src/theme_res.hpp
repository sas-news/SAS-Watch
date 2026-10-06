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
#include "watch/theme/manifest.hpp"

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

// ---- v3: skin (9-slice コンポーネント画像) ----

// 9-slice に分割したスキン画像。reg の並び:
//   0:左上 1:上辺 2:右上 / 3:左辺 4:中央 5:右辺 / 6:左下 7:下辺 8:右下
// slice が全 0 のときは reg[4] のみ使い全面伸縮。
struct SkinImg {
  lv_image_dsc_t reg[9];
  uint8_t slice[4];   // 実際に使った l,t,r,b (clamp 済)
};

// 1パーツ分の state 画像セット。index: 0=normal 1=pressed 2=checked
// 3=disabled (ThemeSkinStateId +1)。ロード失敗/未指定は nullptr。
struct SkinSet {
  const SkinImg* img[4];
};

// manifest の skin_part 定義から画像を全てロードする。
// 失敗時 nullptr (呼び出し側はベクタ描画にフォールバック)。
const SkinSet* skin_load(const char* id, const watch::ThemeSkinPart& part);

// 今のテーマで skin 用に確保したアリーナ領域の合計バイト数。
uint32_t skin_bytes();

// ロード済みフォントと画像 dsc プールを全て解放する
// (port::theme_assets_reset の直前に必須)。
void reset();

}  // namespace theme_res
