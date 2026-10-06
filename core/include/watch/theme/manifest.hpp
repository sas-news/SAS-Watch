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
  FaceChara,  // 文字盤 chara_* 用の立ち絵 (透過、最大 240x410)
};
constexpr int kThemeImageSlots = 4;
constexpr size_t kThemeImageNameMax = 24;  // "timer_done.bin" まで

// ---- v2 拡張 (manifest の追加キー。全部任意) ----------------------------

// "screens" の画面キー表。index は ThemeManifest::screens[] と
// mascot.screens / Theme のマスクと一致する。"alert" は全画面アラート
// (タイマー終了/アラーム鳴動) のオーバーレイ。"*" は screens_set の
// kThemeScreenWildcard で別途保持 (全画面フォールバック)。
constexpr int kThemeScreenCount = 16;
constexpr int kThemeScreenWildcard = 31;
extern const char* const kThemeScreenNames[kThemeScreenCount];

// 画面名 → kThemeScreenNames の添字 ("*" → kThemeScreenWildcard、未知 → -1)。
// manifest パーサと ui 側の画面→スキン解決で共通。
int theme_screen_index(const char* name);

// 画面ごとの背景+スクリム。
struct ThemeScreenSkin {
  char bg[kThemeImageNameMax];  // 画像ファイル名 (.bin / .png)
  uint8_t scrim = 0;            // 黒敷きの不透明度 0-255
};

// "style" のコンポーネントスキン。
struct ThemeStyleSkin {
  uint8_t card_radius = 0;   // group/card の角丸 (省略時 tokens.radius_lg)
  uint8_t card_opa = 0;      // カード不透明度 0-255 (省略=255)
  uint8_t border_w = 0;      // カード枠線の太さ px
  uint8_t btn_radius = 0;    // ボタン角丸 (省略=ピル)
  uint8_t glow_w = 0;        // primary ボタンのグロー (影) 幅
  uint8_t header = 0;        // 0=default 1=flat (戻るピル無し)
  uint32_t border = 0;       // カード枠色 0xRRGGBB
  uint32_t glow = 0;         // グロー色
  uint16_t set = 0;          // bit i = 上記フィールド順に指定あり
};
// set ビット番号 (ThemeStyleSkin::set)
enum ThemeStyleBit : uint8_t {
  kStyleCardRadius = 0, kStyleCardOpa, kStyleBorderW, kStyleBtnRadius,
  kStyleGlowW, kStyleHeader, kStyleBorder, kStyleGlow,
};

// "icons": アプリ id → 画像ファイル名 (アプリ一覧のタイル差替え)。
constexpr int kThemeMaxIcons = 12;
constexpr size_t kThemeAppIdMax = 16;
struct ThemeIcon {
  char app[kThemeAppIdMax];
  char file[kThemeImageNameMax];
};

// "fonts": スロット → LVGL バイナリフォント (.bin、lv_binfont 形式)。
constexpr int kThemeFontSlotCount = 4;
constexpr const char* const kThemeFontKeys[kThemeFontSlotCount] = {
    "body", "title", "digits", "digits_sm"};

// "mascot": 画面隅のマスコット。
constexpr int kThemeMascotExprMax = 4;
constexpr int kThemeMascotLineMax = 8;
constexpr size_t kThemeMascotLineMaxLen = 48;
struct ThemeMascot {
  // expr: 表情名 → 画像ファイル (最大4種)
  char expr_name[kThemeMascotExprMax][16] = {};
  char expr_file[kThemeMascotExprMax][kThemeImageNameMax] = {};
  uint8_t expr_count = 0;
  // lines: タップでランダム表示のセリフ (utf-8)
  char line[kThemeMascotLineMax][kThemeMascotLineMaxLen] = {};
  uint8_t line_count = 0;
  int16_t x = 0;              // 画像の左上 x (画面座標)
  int16_t y = 0;              // 画像の左上 y
  uint32_t screens = 0;       // bit i = kThemeScreenNames[i] で有効
  uint8_t used = 0;           // mascot キー自体の有無
};

// "face_layout": テーマ文字盤 (id "theme") の要素配置。
// elem = time/date/steps/battery/notify/bubble/chara。
constexpr int kThemeFaceElemCount = 7;
extern const char* const kThemeFaceElemNames[kThemeFaceElemCount];
// face_layout_set / ThemeFaceElem の添字 (kThemeFaceElemNames と同順)。
enum ThemeFaceElemId : uint8_t {
  kThemeFaceElemTime = 0, kThemeFaceElemDate, kThemeFaceElemSteps,
  kThemeFaceElemBattery, kThemeFaceElemNotify, kThemeFaceElemBubble,
  kThemeFaceElemChara,
};
struct ThemeFaceElem {
  int16_t x = 0;
  int16_t y = 0;
  int16_t w = 0;             // 幅 (0=内容に合わせる)
  int16_t h = 0;
  int16_t font = 0;          // 数字フォント px (0=既定)
  uint32_t color = 0;        // 0xRRGGBB
  char img[kThemeImageNameMax] = {};  // chara 用画像 (省略時 images.face_chara)
  uint8_t set = 0;           // bit0 x,1 y,2 w,3 h,4 font,5 color,6 img
};

struct ThemeManifest {
  char id[32] = {};
  char name[48] = {};
  uint32_t version = 1;
  uint32_t api = 0;

  // ---- tokens: 未指定キーは *_set ビットが 0 のまま (適用側は標準値で補完)。
  // 色は parse 済み 0xRRGGBB。順序は docs/theme-format.md の tokens 表。
  uint32_t color[19] = {};
  uint32_t color_set = 0;        // bit i = color[i] が指定された
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

  // chara_bubble 文字盤のふきだし文言 (任意)。
  // 0:朝 1:昼 2:夕 3:夜 4:歩数 ({n} は残り歩数に置換)。
  char bubble[5][48] = {};
  uint8_t bubble_set = 0;        // bit i = bubble[i] が指定された

  // ---- v2 拡張 ----
  // screens[i] が埋まった = screens_set bit i。"*" 指定時は
  // wildcard_skin に入って screens_set の bit kThemeScreenWildcard が立つ。
  ThemeScreenSkin screens[kThemeScreenCount];
  ThemeScreenSkin wildcard_skin;
  uint32_t screens_set = 0;

  ThemeStyleSkin style;

  ThemeIcon icons[kThemeMaxIcons];
  uint8_t icon_count = 0;

  // fonts[slot] = ファイル名 (font_file_set bit i で指定あり)。
  // スロット順は kThemeFontKeys と同じ (body/title/digits/digits_sm)。
  char font_file[4][kThemeImageNameMax] = {};
  uint8_t font_file_set = 0;

  ThemeMascot mascot;

  // face_layout[elem] が埋まった = face_layout_set bit i。
  // 全て空なら "theme" 文字盤は既定レイアウト。
  ThemeFaceElem face_elem[kThemeFaceElemCount];
  uint8_t face_layout_set = 0;
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
