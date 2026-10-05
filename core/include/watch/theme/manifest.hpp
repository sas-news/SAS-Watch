// theme/manifest.hpp — テーマパッケージ manifest.cbor の読み取り。
// 形式は docs/theme-format.md。LVGL/ESP-IDF には依存しない。
#pragma once

#include <cstddef>
#include <cstdint>

namespace watch {

// theme id: [a-z0-9-]{1,31} (Settings.theme[32] に収まる)。
bool theme_id_ok(const char* id);

// 画像スロット (docs/theme-format.md 参照)。
enum class ThemeImageSlot : uint8_t {
  HomeBg = 0,
  Stand,
  TimerDone,
};
constexpr int kThemeImageSlots = 3;
constexpr size_t kThemeImageNameMax = 24;  // "timer_done.bin" まで

struct ThemeManifest {
  char id[32] = {};
  char name[48] = {};
  uint32_t version = 1;
  uint32_t api = 0;

  // ---- tokens: 未指定キーは *_set ビットが 0 のまま (適用側は標準値で補完)。
  // 色は parse 済み 0xRRGGBB。順序は docs/theme-format.md の tokens 表。
  uint32_t color[10] = {};
  uint16_t color_set = 0;        // bit i = color[i] が指定された
  uint8_t radius_sm = 0;
  uint8_t radius_lg = 0;
  uint8_t space = 0;
  uint16_t anim_ms = 0;
  uint8_t metric_set = 0;        // bit0-3 = radius_sm/lg/space/anim_ms
  // フォントは px サイズ (内蔵 {20,26,56,96} から選ぶ)。
  uint8_t font_px[4] = {};       // body/title/digits/digits_sm
  uint8_t font_set = 0;

  // images[slot] = zip 内ファイル名 (指定時のみ)。
  char image[kThemeImageSlots][kThemeImageNameMax] = {};
  uint8_t image_set = 0;
};

enum class ThemeManifestError : uint8_t {
  kOk = 0,
  kBadCbor,        // CBOR として読めない
  kMissingId,      // id が無い
  kBadId,          // theme_id_ok() に合わない
  kBadApi,         // api != 1
  kBadValue,       // 色/数値/フォント値の型・形式が違う
  kBadImageName,   // 画像ファイル名が危険/長すぎる
};

// cbor[n] を解析して out に書く。失敗時 out は未定義。
ThemeManifestError theme_manifest_parse(const uint8_t* cbor, size_t n,
                                        ThemeManifest* out);

}  // namespace watch
