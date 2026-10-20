// components.cpp — 共通部品の実装。
//   見た目は docs/design/ui.html のモックアップに合わせる:
//   - 黒背景 + 角丸グループ (surface) に行を区切り線で並べる
//   - 左右 32px (実測角R≈100px の内側)、ヘッダ 76px、ボタンは高 58px のピル
//   - 色・角丸・フォントは Theme トークンから取る (file テーマも同じ形)。
#include "components.hpp"

#include "theme_res.hpp"
#include "ui/port.hpp"

#if defined(ESP_PLATFORM)
#include "esp_timer.h"
#else
#include <cstdio>
#include <cstdlib>
#endif

// ‹ › (U+2039/203A) だけの小さなフォント。font_jp_min_20 は既定収録範囲外で
// 再生成すると既存グリフが全部入れ替わるので、シェブロン専用に分けた。
LV_FONT_DECLARE(font_chev_22);
LV_FONT_DECLARE(font_icon_36);

namespace ui::c {

namespace {

constexpr lv_coord_t kW = 410;     // 表示幅
constexpr lv_coord_t kPad = 32;    // コンテンツの左右余白 (実測角R≈100px の内側)
constexpr lv_coord_t kHeaderH = 76;
constexpr lv_coord_t kBackH = 48;  // 戻るピル高さ
constexpr lv_coord_t kRowMinH = 60;
constexpr lv_coord_t kRowPadX = 16;
constexpr lv_coord_t kRowGap = 14;
constexpr lv_coord_t kIconTile = 36;
constexpr lv_coord_t kBtnH = 58;
constexpr lv_coord_t kSwitchW = 52;
constexpr lv_coord_t kSwitchH = 30;
constexpr lv_coord_t kTrackH = 10;
constexpr lv_coord_t kKnob = 24;
// スライダーのノブ外径。テーマの knob 画像は「24px の円 + 3px の外周
// リング」(=30px) を描きたいので領域も 30 に合わせる (T27-d)。
constexpr lv_coord_t kSliderKnob = 30;

// group() で作ったオブジェクトを区別するマーカー (slider_row が
// 「グループ内のブロック」か「自分のグループ」かを決めるのに使う)。
void* const kGroupTag = const_cast<char*>("ui::c::group");

// header() で作ったヘッダのマーカー (mascot が小型表示か隅表示かを
// 判別するために使う)。
void* const kHeaderTag = const_cast<char*>("ui::c::header");

// mk_label() で作ったラベルのマーカー (skin の text/text_pressed で
// 色を差し替える対象の識別に使う)。
void* const kLabelTag = const_cast<char*>("ui::c::label");

// v3 スキン適用ヘルパ (このファイル後半の 9-slice 実装)。
// 画像があれば true。
bool skin_apply_obj(lv_obj_t* o, watch::ThemeSkinPartId part, int mode,
                    bool use_pad);

// CLICKED は画面ルートまでバブルするので、ルートの cb 1つで
// 全ボタン/行タップのクリック音を拾える。
void on_any_click(lv_event_t*) { ui::port::click(); }

void bubble(lv_obj_t* o) { lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE); }

void clickable(lv_obj_t* o) {
  lv_obj_add_flag(
      o, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE |
                                    LV_OBJ_FLAG_EVENT_BUBBLE));
}

// フラットな子コンテナ (枠なし・透過・スクロール無し)。
lv_obj_t* flat(lv_obj_t* parent) {
  lv_obj_t* o = lv_obj_create(parent);
  bubble(o);
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

lv_obj_t* mk_label(lv_obj_t* parent, const char* text,
                   const lv_font_t* font, lv_color_t col) {
  lv_obj_t* l = lv_label_create(parent);
  bubble(l);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, col, 0);
  // 本文の行間はフォント高の 1.25 倍 (T4)。
  lv_obj_set_style_text_line_space(
      l, lv_font_get_line_height(font) / 4, 0);
  // スキンの text/text_pressed で塗り替える対象としてマーク。
  lv_obj_set_user_data(l, kLabelTag);
  return l;
}

// グループ内でアイテム (行・スライダーブロック) の前に区切り線を挿す。
void divider_if_needed(lv_obj_t* grp) {
  const Theme& t = theme();
  if (lv_obj_get_child_count(grp) == 0) return;
  lv_obj_t* d = flat(grp);
  lv_obj_set_size(d, LV_PCT(100), 1);
  lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(d, t.line, 0);
  skin_apply_obj(d, watch::kSkinDivider, 0, false);
}

// 右端の › (モックの .ch #4A4F5C)。専用フォントで描く。
lv_obj_t* chevron(lv_obj_t* row) {
  const Theme& t = theme();
  lv_obj_t* ch = mk_label(row, "›", &font_chev_22, t.edge);
  return ch;
}

// ---- v2 style スキン (ThemeStyleSkin::set ビットが指定フラグ) ----

lv_coord_t skin_card_radius() {
  const Theme& t = theme();
  return (t.style.set & (1u << watch::kStyleCardRadius))
             ? t.style.card_radius
             : t.radius_lg;
}

lv_coord_t skin_btn_radius() {
  const Theme& t = theme();
  return (t.style.set & (1u << watch::kStyleBtnRadius))
             ? t.style.btn_radius
             : LV_RADIUS_CIRCLE;
}

// group/card の「カードっぽさ」をまとめて適用。
void skin_card(lv_obj_t* o) {
  const Theme& t = theme();
  lv_obj_set_style_radius(o, skin_card_radius(), 0);
  lv_obj_set_style_bg_color(o, t.surface, 0);
  lv_obj_set_style_bg_opa(
      o,
      (t.style.set & (1u << watch::kStyleCardOpa)) ? t.style.card_opa
                                                   : LV_OPA_COVER,
      0);
  // カード枠: 内蔵テーマは 1px の line 色。style.border_w/border が
  // 指定されていればそちらを優先 (LVGL 既定の白枠を残さない)。
  const uint8_t bw = (t.style.set & (1u << watch::kStyleBorderW))
                         ? t.style.border_w
                         : 1;
  lv_obj_set_style_border_width(o, bw, 0);
  if (bw) {
    lv_obj_set_style_border_color(
        o,
        (t.style.set & (1u << watch::kStyleBorder))
            ? lv_color_hex(t.style.border)
            : t.line,
        0);
  }
  // glow: カードの影 (glow_w 指定時のみ)。描画負荷を抑えるため
  // テーマ値も 6px までにクランプする (roadmap T5)。
  if (t.style.set & (1u << watch::kStyleGlowW)) {
    lv_obj_set_style_shadow_width(o, LV_MIN(t.style.glow_w, 6), 0);
    lv_obj_set_style_shadow_color(
        o,
        (t.style.set & (1u << watch::kStyleGlow))
            ? lv_color_hex(t.style.glow)
            : t.primary,
        0);
    lv_obj_set_style_shadow_opa(o, 120, 0);
    lv_obj_set_style_shadow_spread(o, 0, 0);
  }
}

// ---- v3 画像スキン (manifest "skin" の 9-slice) ----

// LV_STATE → SkinSet::img の index。
int skin_state_idx(lv_state_t st) {
  if (st & LV_STATE_DISABLED) return 3;
  if (st & LV_STATE_CHECKED) return 2;
  if (st & LV_STATE_PRESSED) return 1;
  return 0;
}

// reg 画像を (x,y,w,h) に伸縮して描画。lv_draw_image は coords を
// 「拡大前の画像の置き場所」として解釈する (変換は pivot 起点) ので、
// coords には src サイズの矩形を渡す。
// scale は floor ではなく ceil を使う: LVGL は描画幅を
// (src*scale)>>8 の切り捨てで決めるため floor だと要求幅より最大1px
// 足りず、領域境に未描画の1px抜け線が出る。ceil による最大1pxの
// はみ出しは、後で描かれる隣接領域が上書きするので見えない。
void skin_draw_reg(lv_layer_t* layer, const lv_image_dsc_t* d,
                   int32_t x, int32_t y, int32_t w, int32_t h) {
  if (!d || !d->data || !d->header.w || !d->header.h || w <= 0 || h <= 0) {
    return;
  }
  lv_draw_image_dsc_t dsc;
  lv_draw_image_dsc_init(&dsc);
  dsc.src = d;
  dsc.scale_x = static_cast<int32_t>(
      (w * LV_SCALE_NONE + static_cast<int32_t>(d->header.w) - 1) /
      static_cast<int32_t>(d->header.w));
  dsc.scale_y = static_cast<int32_t>(
      (h * LV_SCALE_NONE + static_cast<int32_t>(d->header.h) - 1) /
      static_cast<int32_t>(d->header.h));
  dsc.pivot = {0, 0};
  lv_area_t a = {x, y, x + d->header.w - 1, y + d->header.h - 1};
  lv_draw_image(layer, &dsc, &a);
}

// ---- 大きい中央領域の高速近似 ----
// 全面級のカード (list_group 等) は 9-slice の中央伸縮だけで毎フレーム
// ~20万px のソフト変換になり支配的だった (実機計測)。カード内部の絵は
// 実質 1 軸のなだらかなグラデーションなので、出力領域が大きいときは
// 中央を画像ではなく 2-stop 線形グラデの矩形塗りで近似する。端点は
// 実画像の中央領域の隅から採るので誤差は小さい (cute は誤差 0)。
// α が均一でない領域や、縦横とも強い変化 (模様・対角) は画像に戻す。

// src (RGB565/RGB565A8) の (px,py) を読む。
void skin_px_at(const lv_image_dsc_t* d, int32_t px, int32_t py,
                lv_color_t* col, lv_opa_t* opa) {
  const uint32_t stride =
      d->header.stride ? d->header.stride
                       : static_cast<uint32_t>(d->header.w) * 2;
  const uint16_t* row =
      reinterpret_cast<const uint16_t*>(d->data + py * stride);
  const uint16_t v = row[px];
  *col = lv_color_make(static_cast<uint8_t>((v >> 8) & 0xF8),
                       static_cast<uint8_t>((v >> 3) & 0xFC),
                       static_cast<uint8_t>((v << 3) & 0xF8));
  *opa = d->header.cf == LV_COLOR_FORMAT_RGB565A8
             ? static_cast<lv_opa_t>(
                   d->data[stride * d->header.h +
                           static_cast<uint32_t>(py) * (stride / 2) + px])
             : static_cast<lv_opa_t>(LV_OPA_COVER);
}

int skin_px_diff(lv_color_t a, lv_color_t b) {
  return LV_ABS(a.red - b.red) + LV_ABS(a.green - b.green) +
         LV_ABS(a.blue - b.blue);
}

uint32_t s_ss_fill_n = 0, s_ss_band_n = 0;  // 近似パス領域数/帯 draw call 数
uint32_t s_ss_imgreg_n = 0;                 // 画像パス領域数

struct SkinRun;
bool skin_runs_draw(lv_layer_t* layer, const lv_image_dsc_t* d, int32_t x,
                    int32_t y, int32_t w, int32_t h, bool use_h,
                    const lv_color_t* col, lv_opa_t opa,
                    const SkinRun* runs, int nr, int linerr);
// 近似パスを断った理由別 (fmt=非565/小, scan=走査超過, alpha=α不均一,
// nonuni=直交非一様, runs=帯>64, bands=非ベース帯>40)
uint32_t s_ss_bail[6] = {0};

// 領域の中央を走査し、1 軸の変化として表せるなら矩形塗りの列で
// 描画する (ソフト変換回避)。戻り値 true = 描画済み。
// 判定は実ピクセルベースなので、近似できない絵は画像描画に戻る:
//   - 単色 → 塗り1発 (cute のカード中央はこれで誤差 0)
//   - 滑らかなグラデ (線形近似の誤差が小さい) → 2-stop グラデ
//   - 2色以上の縞/帯 (cyber のスキャンライン等) → run 毎の帯塗り
//   - それ以外 (α不均一・横方向に非一様・細かい模様) → false
constexpr int kSkinScanMax = 640;  // 走査上限 (skin 領域は数百px)

struct SkinRun {
  uint16_t s, e;     // src 行/列範囲 [s, e] (kSkinScanMax=640 に収まる)
  lv_color_t c;
};

// axis=false: 中央列を縦に走査 (各行が横一様か確認)
// axis=true : 中央行を横に走査 (各列が縦一様か確認)
bool skin_1d_scan(const lv_image_dsc_t* d, bool horiz,
                  lv_color_t* col, lv_opa_t* opa, SkinRun* runs,
                  int* n_runs, int* max_err_vs_linear) {
  const int32_t sw = d->header.w, sh = d->header.h;
  const int32_t n = horiz ? sw : sh;
  if (n > kSkinScanMax) {
    ++s_ss_bail[1];
    return false;
  }
  const int32_t mid = (horiz ? sh : sw) / 2;
  lv_opa_t a0 = 0;
  bool ainit = false;
  // 中央ラインを取得しつつ、直交方向の一様性と α の均一さを数か所確認。
  const int nq = 6;
  for (int32_t i = 0; i < n; ++i) {
    lv_color_t c;
    lv_opa_t a;
    skin_px_at(d, horiz ? i : mid, horiz ? mid : i, &c, &a);
    if (!ainit) {
      a0 = a;
      ainit = true;
    } else if (LV_ABS(a - a0) > 8) {
      ++s_ss_bail[2];
      return false;  // 内部 α が均一でない
    }
    col[i] = c;
  }
  // 直交方向の一様性: nq 個の位置で、ライン両端 vs 中央を比較。
  for (int k = 0; k < nq; ++k) {
    const int32_t t = (n - 1) * k / (nq - 1);
    lv_color_t ca, cb, cc;
    lv_opa_t oa, ob, oc;
    skin_px_at(d, horiz ? t : 0, horiz ? 0 : t, &ca, &oa);
    skin_px_at(d, horiz ? t : sw - 1, horiz ? sh - 1 : t, &cb, &ob);
    skin_px_at(d, horiz ? t : mid, horiz ? mid : t, &cc, &oc);
    if (skin_px_diff(ca, cc) > 12 || skin_px_diff(cb, cc) > 12) {
      ++s_ss_bail[3];
      return false;
    }
    if (LV_ABS(oa - a0) > 8 || LV_ABS(ob - a0) > 8) {
      ++s_ss_bail[2];
      return false;
    }
  }
  *opa = a0;
  // run 圧縮 (色ずれ ±4 を同色とみなす)。
  int nr = 0;
  for (int32_t i = 0; i < n;) {
    int32_t j = i + 1;
    while (j < n && skin_px_diff(col[j], col[i]) <= 12) ++j;
    if (nr < 64) runs[nr] = {static_cast<uint16_t>(i),
                             static_cast<uint16_t>(j - 1), col[i]};
    ++nr;
    i = j;
  }
  *n_runs = nr;
  // 線形近似誤差 (端点は lerp するので中点付近だけ見る)。
  int err = 0;
  for (int32_t i = 1; i < n - 1; ++i) {
    const int t = i * 255 / (n - 1);
    const lv_color_t g = {
        static_cast<uint8_t>(col[0].blue +
                             (col[n - 1].blue - col[0].blue) * t / 255),
        static_cast<uint8_t>(col[0].green +
                             (col[n - 1].green - col[0].green) * t / 255),
        static_cast<uint8_t>(col[0].red +
                             (col[n - 1].red - col[0].red) * t / 255)};
    err = LV_MAX(err, skin_px_diff(col[i], g));
    if (err > 40) break;  // 打ち切り
  }
  *max_err_vs_linear = err;
  return true;
}

void skin_fill_rect(lv_layer_t* layer, int32_t x, int32_t y, int32_t w,
                    int32_t h, lv_color_t c, lv_opa_t opa) {
  lv_draw_rect_dsc_t rd;
  lv_draw_rect_dsc_init(&rd);
  rd.bg_opa = opa;
  rd.bg_color = c;
  const lv_area_t a = {x, y, x + w - 1, y + h - 1};
  lv_draw_rect(layer, &rd, &a);
}

bool skin_region_fill(lv_layer_t* layer, const lv_image_dsc_t* d,
                      int32_t x, int32_t y, int32_t w, int32_t h) {
#ifndef ESP_PLATFORM
  if (getenv("SKIN_FILL_OFF")) return false;
#endif
  if (!d || !d->data || d->header.w < 2 || d->header.h < 2) {
    ++s_ss_bail[0];
    return false;
  }
  if (d->header.cf != LV_COLOR_FORMAT_RGB565 &&
      d->header.cf != LV_COLOR_FORMAT_RGB565A8) {
    ++s_ss_bail[0];
    return false;
  }
  static lv_color_t col[kSkinScanMax];
  static SkinRun runs[64];
  lv_opa_t opa = 0;
  int nr = 0, linerr = 0;
  // 走査軸は変化が支配的な方向に合わせる必要がある (横長領域でも
  // 縦方向の縞模様なら縦走査でしか表せない)。形状から推した軸を
  // 先に試す。細い縁ストリップは LVGL の伸縮補間そのものが見た目の
  // 一部 (端のフェード等) なので、両辺が大きい領域だけ逆軸も試す。
  bool use_h = d->header.w >= 4 * d->header.h;
  if (!skin_1d_scan(d, use_h, col, &opa, runs, &nr, &linerr)) {
    if (w < 64 || h < 64) return false;
    use_h = !use_h;
    if (!skin_1d_scan(d, use_h, col, &opa, runs, &nr, &linerr)) {
      return false;
    }
  }
  return skin_runs_draw(layer, d, x, y, w, h, use_h, col, opa, runs, nr,
                        linerr);
}

// 1D 走査結果 (col/nr/linerr) に基づいて領域を塗る。
// true = 描画済み, false = 表せないので画像描画に戻る。
bool skin_runs_draw(lv_layer_t* layer, const lv_image_dsc_t* d, int32_t x,
                    int32_t y, int32_t w, int32_t h, bool use_h,
                    const lv_color_t* col, lv_opa_t opa,
                    const SkinRun* runs, int nr, int linerr) {
#ifndef ESP_PLATFORM
  if (getenv("SKIN_FILL_DEBUG")) {
    fprintf(stderr, "  scan n=%d nr=%d linerr=%d use_h=%d opa=%d c0=(%d,%d,%d) cN=(%d,%d,%d)\n",
            use_h ? (int)d->header.w : (int)d->header.h, nr, linerr,
            (int)use_h, (int)opa, col[0].red, col[0].green, col[0].blue,
            col[(use_h ? (int)d->header.w : (int)d->header.h) - 1].red,
            col[(use_h ? (int)d->header.w : (int)d->header.h) - 1].green,
            col[(use_h ? (int)d->header.w : (int)d->header.h) - 1].blue);
    for (int i = 0; i < nr && i < 16; ++i) {
      fprintf(stderr, "    run[%d] %d..%d (%d,%d,%d)\n", i, runs[i].s,
              runs[i].e, runs[i].c.red, runs[i].c.green, runs[i].c.blue);
    }
  }
#endif

  if (nr == 1) {
    skin_fill_rect(layer, x, y, w, h, col[0], opa);
    return true;
  }
  // 滑らかなグラデーション → 2-stop グラデ矩形。
  if (linerr <= 12) {
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_opa = opa;
    rd.bg_grad.dir = use_h ? LV_GRAD_DIR_HOR : LV_GRAD_DIR_VER;
    rd.bg_grad.stops_count = 2;
    rd.bg_grad.stops[0] = {col[0], LV_OPA_COVER, 0};
    const int32_t n = use_h ? d->header.w : d->header.h;
    rd.bg_grad.stops[1] = {col[n - 1], LV_OPA_COVER, 255};
    const lv_area_t a = {x, y, x + w - 1, y + h - 1};
    lv_draw_rect(layer, &rd, &a);
    return true;
  }
  // 縞/帯: 全帯を塗ると draw call が帯数分掛かるので、最多色を
  // 全面に敷いて残りの帯だけ重ねる (交互縞なら約半分に減る)。
  if (nr > 64) {
    ++s_ss_bail[4];
    return false;
  }
  const int32_t n = use_h ? d->header.w : d->header.h;
  // 最多出現の run 色をベースに選ぶ。
  int base = 0, best = 0;
  for (int i = 0; i < nr; ++i) {
    const int wgt = runs[i].e - runs[i].s + 1;
    if (wgt > best) {
      best = wgt;
      base = i;
    }
  }
  int overlap = 0;
  for (int i = 0; i < nr; ++i) {
    if (i != base && skin_px_diff(runs[i].c, runs[base].c) <= 12) ++overlap;
  }
  // ベース以外の帯数 (= draw call -1)。帯の反復が細かい絵は
  // draw call コストが変換を上回るので画像に戻す。
  if (nr - overlap > 40) {
    ++s_ss_bail[5];
    return false;
  }
  s_ss_band_n += static_cast<uint32_t>(nr - overlap) + 1;
  skin_fill_rect(layer, x, y, w, h, runs[base].c, opa);
  for (int i = 0; i < nr; ++i) {
    if (i == base || skin_px_diff(runs[i].c, runs[base].c) <= 12) continue;
    const int32_t b0 = static_cast<int32_t>(
        (static_cast<int64_t>(runs[i].s) * (use_h ? w : h)) / n);
    const int32_t b1 = static_cast<int32_t>(
        (static_cast<int64_t>(runs[i].e + 1) * (use_h ? w : h)) / n) - 1;
    if (b1 < b0) continue;
    if (use_h) {
      skin_fill_rect(layer, x + b0, y, b1 - b0 + 1, h, runs[i].c, opa);
    } else {
      skin_fill_rect(layer, x, y + b0, w, b1 - b0 + 1, runs[i].c, opa);
    }
  }
  return true;
}

// SkinImg の 9 領域を area に配置して描画 (角=原寸、辺/中央=伸縮)。
void skin_draw_9(lv_layer_t* layer, const theme_res::SkinImg* im,
                 const lv_area_t* a) {
  int32_t l = im->slice[0];
  int32_t tp = im->slice[1];
  int32_t r = im->slice[2];
  int32_t b = im->slice[3];
  const int32_t w = lv_area_get_width(a);
  const int32_t h = lv_area_get_height(a);
  if (w <= 0 || h <= 0) return;
  // 対象が slice より小さいときは潰れないよう縮める。
  if (l + r > w) {
    l = w / 2;
    r = w - l;
  }
  if (tp + b > h) {
    tp = h / 2;
    b = h - tp;
  }
  const int32_t xs[4] = {a->x1, a->x1 + l, a->x2 - r + 1, a->x2 + 1};
  const int32_t ys[4] = {a->y1, a->y1 + tp, a->y2 - b + 1, a->y2 + 1};
  for (int i = 0; i < 9; ++i) {
    const lv_image_dsc_t* d = &im->reg[i];
    if (!d->data) continue;
    const int ry = i / 3;
    const int cx = i % 3;
    const int32_t rw = xs[cx + 1] - xs[cx];
    const int32_t rh = ys[ry + 1] - ys[ry];
    // 角以外の大きめ領域は線形グラデの矩形塗りで近似 (ソフト変換回避)。
    // 縁ストリップも実質 1 軸グラデなので同じ近似が効く。近似条件
    // (α均一・1軸優勢) を満たさない絵は画像描画に戻るので安全。
    const bool corner = (ry != 1 && cx != 1);
    if (!corner && static_cast<int64_t>(rw) * rh >= 2048 &&
        skin_region_fill(layer, d, xs[cx], ys[ry], rw, rh)) {
      ++s_ss_fill_n;
#ifndef ESP_PLATFORM
      if (getenv("SKIN_FILL_DEBUG")) {
        fprintf(stderr, "fill9 %p reg%d out %d,%d %dx%d src %ux%u\n",
                (const void*)im, i, (int)xs[cx], (int)ys[ry], rw, rh,
                d->header.w, d->header.h);
      }
#endif
      continue;
    }
    ++s_ss_imgreg_n;
    skin_draw_reg(layer, d, xs[cx], ys[ry], rw, rh);
  }
}

// ---- スキン 9-slice レンダーキャッシュ ----
// 9-slice は領域ごとの伸縮描画で LVGL のソフト変換パスを毎フレーム通り
// 重い (実機 cute の settings で全画面 ~250ms の ~65%)。スキン画像と
// 表示サイズが同じなら見た目は不変なので (SkinImg*, w, h) をキーに
// RGB565A8 の完成形を 1 回だけ作り、以後は scale=1 のブリットで描く。
// (cf は RGB565A8: 色面が RGB565 量子化のままなので往復変換で 1bit も
//  ずれず、縁のアンチエイリアス α も保てる)。
//   - 最大 16 エントリ・合計 512KB (アプリ一覧の bake 用 ~850KB と
//     PSRAM を取り合うので控えめ)。超過は、直近フレームで描いていない
//     ものを古い順 (LRU) に追い出す。表示中のものしか無ければ諦める。
//   - 構築は描画イベントの中では行わない: ミス時は従来通り伸縮描画し
//     つつ lv_async_call で非描画タイミングに構築を予約する (DRAW
//     イベント中に別の canvas/layer を使って描画しないため)。
//   - テーマ差し替えで SkinImg* が無効になるため theme_res::reset()
//     から ui::c::skin_cache_clear() で全破棄する。

constexpr int kSkinCacheMax = 16;
constexpr uint32_t kSkinCacheBytes = 512 * 1024;
// これより大きい見え面はキャッシュしない。大きな部品はキャッシュの
// RGB565A8 ブリット (read+α合成+write) より、直接描画 (縁の伸縮 +
// 中央のグラデ矩形は dst に書くだけ) のほうが速く、キャッシュ容量も
// 小さな部品のために残せる (settings の 6 万px カードで測定)。
constexpr int32_t kSkinCacheMaxPx = 32768;

// ---- スキン描画の内訳計測 (ESP 実機のみ。`b` ベンチの skinstats: 行) ----
// 注意: LVGL9 の描画はタスク化されていて lv_draw_image の呼び出し時間は
// 実描画コストを含まない。ここでは「呼数・種別・拒否理由」を数えるに留める。
int64_t stat_now_us() {
#if defined(ESP_PLATFORM)
  return esp_timer_get_time();
#else
  return 0;
#endif
}
uint64_t s_ss_blit_us = 0, s_ss_miss_us = 0;
uint32_t s_ss_blit_n = 0, s_ss_miss_n = 0;
uint32_t s_ss_blit_px = 0, s_ss_miss_px = 0;
uint32_t s_ss_rej_size = 0, s_ss_rej_room = 0;  // キャッシュ不能/満杯の要求数
uint16_t s_ss_rej_w = 0, s_ss_rej_h = 0;    // 最後に拒否した部品の表示サイズ
uint16_t s_ss_rej_sw = 0, s_ss_rej_sh = 0;  // その中心領域のソースサイズ

struct SkinCacheEnt {
  const theme_res::SkinImg* im = nullptr;
  lv_draw_buf_t* buf = nullptr;
  uint16_t w = 0;
  uint16_t h = 0;
  uint32_t age = 0;
  uint32_t frame = 0;  // 最後に描いたフレーム (s_frame)
};
SkinCacheEnt s_scache[kSkinCacheMax];
uint32_t s_scache_age = 0;

// 構築予約 (LVGL タスク内でのみ触る)。
struct SkinCacheReq {
  const theme_res::SkinImg* im;
  uint16_t w;
  uint16_t h;
};
SkinCacheReq s_screq[kSkinCacheMax];
int s_screq_n = 0;
bool s_scbuild_sched = false;

// 描画フレーム番号 (LV_EVENT_RENDER_START で進める)。直近のフレームで
// 描いたエントリは表示中とみなし、追い出さない。追い出すと、収まり
// きらない画面で「構築→追い出し→再描画でミス→構築」が回り続けて
// LVGL タスクを占有する (実機の cute 設定画面で発生)。
uint32_t s_frame = 0;
bool s_frame_hooked = false;

void frame_cb(lv_event_t*) { ++s_frame; }

// need バイト分を置けるよう、直近フレームで使っていない古いエントリから
// 破棄する。表示中のものを消さないと収まらないなら false (この要求は諦める)。
bool scache_make_room(uint32_t need) {
  for (;;) {
    uint32_t total = 0;
    int n = 0;
    int oldest = -1;
    for (int i = 0; i < kSkinCacheMax; ++i) {
      if (!s_scache[i].buf) continue;
      ++n;
      total += s_scache[i].buf->data_size;
      if (s_scache[i].frame != s_frame &&
          (oldest < 0 || s_scache[i].age < s_scache[oldest].age)) {
        oldest = i;
      }
    }
    if (total + need <= kSkinCacheBytes && n < kSkinCacheMax) return true;
    if (oldest < 0) return false;
    lv_draw_buf_destroy(s_scache[oldest].buf);
    s_scache[oldest] = SkinCacheEnt{};
  }
}

void scache_put(const theme_res::SkinImg* im, uint16_t w, uint16_t h,
                lv_draw_buf_t* buf) {
  for (auto& e : s_scache) {
    if (!e.buf) {
      e.im = im;
      e.buf = buf;
      e.w = w;
      e.h = h;
      e.age = ++s_scache_age;
      e.frame = s_frame;  // 要求元は直近フレームで表示されていた
      return;
    }
  }
  lv_draw_buf_destroy(buf);  // 空き枠が無い (make_room 済みなので通常来ない)
}

void skin_cache_build(void*) {
  s_scbuild_sched = false;
  // バッチ中だけ使うオフスクリーン canvas。sys レイヤの非表示の子にする
  // (画面切替で消えず、描画にも出ない)。parent=nullptr だと「画面」として
  // 作られ、削除後の layout 更新が display=NULL を踏んで落ちた (実機)。
  lv_obj_t* canvas = lv_canvas_create(lv_layer_sys());
  if (!canvas) {
    s_screq_n = 0;
    return;
  }
  lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN);
  bool built = false;
  while (s_screq_n > 0) {
    const SkinCacheReq r = s_screq[--s_screq_n];
    // 予約中に別経路で作られたものは飛ばす。
    bool have = false;
    for (auto& e : s_scache) {
      if (e.buf && e.im == r.im && e.w == r.w && e.h == r.h) {
        e.age = ++s_scache_age;
        have = true;
        break;
      }
    }
    if (have) continue;
    if (!scache_make_room(static_cast<uint32_t>(r.w) * r.h * 3u + 64u)) {
      ++s_ss_rej_room;  // 予算が足りず構築を見送った
      continue;
    }
    // 合成は ARGB8888 で行う (LVGL の sw blend は RGB565A8 を描画先に
    // すると α 面を書かない)。結果を RGB565A8 に詰め直して保持する。
    lv_draw_buf_t* tmp =
        lv_draw_buf_create(r.w, r.h, LV_COLOR_FORMAT_ARGB8888, 0);
    if (!tmp) continue;
    lv_draw_buf_t* buf =
        lv_draw_buf_create(r.w, r.h, LV_COLOR_FORMAT_RGB565A8, 0);
    if (!buf) {
      lv_draw_buf_destroy(tmp);
      continue;
    }
    lv_draw_buf_clear(tmp, nullptr);
    lv_canvas_set_draw_buf(canvas, tmp);
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    const lv_area_t a = {0, 0, static_cast<lv_coord_t>(r.w - 1),
                         static_cast<lv_coord_t>(r.h - 1)};
    skin_draw_9(&layer, r.im, &a);
    lv_canvas_finish_layer(canvas, &layer);
    // 全面不透明なら RGB565 で保持する (A8 ブレンドより速く、
    // メモリも 2B/px に減る)。
    bool opaque = true;
    for (uint16_t y = 0; y < r.h && opaque; ++y) {
      const uint8_t* s = tmp->data + tmp->header.stride * y;
      for (uint16_t x = 0; x < r.w; ++x) {
        if (s[x * 4 + 3] < 250) {
          opaque = false;
          break;
        }
      }
    }
    if (opaque) {
      lv_draw_buf_t* buf565 =
          lv_draw_buf_create(r.w, r.h, LV_COLOR_FORMAT_RGB565, 0);
      if (buf565) {
        lv_draw_buf_destroy(buf);
        buf = buf565;
      }
    }
    const uint32_t cstride = buf->header.stride;
    const bool has_alpha =
        buf->header.cf == LV_COLOR_FORMAT_RGB565A8;
    uint8_t* alpha = buf->data + cstride * r.h;
    for (uint16_t y = 0; y < r.h; ++y) {
      const uint8_t* s = tmp->data + tmp->header.stride * y;
      uint16_t* c = reinterpret_cast<uint16_t*>(buf->data + cstride * y);
      uint8_t* al = alpha + (cstride / 2) * y;
      for (uint16_t x = 0; x < r.w; ++x) {
        const uint8_t b = s[x * 4 + 0], g = s[x * 4 + 1], rr = s[x * 4 + 2];
        c[x] = static_cast<uint16_t>(((rr & 0xF8) << 8) | ((g & 0xFC) << 3) |
                                     (b >> 3));
        if (has_alpha) al[x] = s[x * 4 + 3];
      }
    }
    lv_draw_buf_destroy(tmp);
    scache_put(r.im, r.w, r.h, buf);
    built = true;
  }
  lv_obj_delete(canvas);
  // 一度のバッチでまとめて再描画させる (キャッシュされたブリットに
  // 切り替わるので次フレームから速くなる)。
  if (built) lv_obj_invalidate(lv_screen_active());
}

void skin_cache_request(const theme_res::SkinImg* im, int32_t w, int32_t h) {
  if (w <= 0 || h <= 0 || w * h > kSkinCacheMaxPx) {
    ++s_ss_rej_size;  // 大きすぎて一生キャッシュされない部品
    s_ss_rej_w = static_cast<uint16_t>(w);
    s_ss_rej_h = static_cast<uint16_t>(h);
    if (im) {
      s_ss_rej_sw = static_cast<uint16_t>(im->reg[4].header.w);
      s_ss_rej_sh = static_cast<uint16_t>(im->reg[4].header.h);
    }
    return;
  }
  for (int i = 0; i < s_screq_n; ++i) {
    if (s_screq[i].im == im && s_screq[i].w == w && s_screq[i].h == h) {
      return;
    }
  }
  if (s_screq_n >= kSkinCacheMax) return;  // 予約が一杯 → 次のミスで再試行
  s_screq[s_screq_n++] = {im, static_cast<uint16_t>(w),
                        static_cast<uint16_t>(h)};
  if (!s_scbuild_sched) {
    s_scbuild_sched = true;
    lv_async_call(skin_cache_build, nullptr);
  }
}

// キャッシュ済みの完成形を area に描く。無ければ構築を予約して false。
bool skin_blit_cached(lv_layer_t* layer, const theme_res::SkinImg* im,
                      const lv_area_t* a) {
  if (!s_frame_hooked) {
    lv_display_add_event_cb(lv_display_get_default(), frame_cb,
                            LV_EVENT_RENDER_START, nullptr);
    s_frame_hooked = true;
  }
  const int32_t w = lv_area_get_width(a);
  const int32_t h = lv_area_get_height(a);
  for (auto& e : s_scache) {
    if (e.buf && e.im == im && e.w == w && e.h == h) {
      e.age = ++s_scache_age;
      e.frame = s_frame;
      lv_draw_image_dsc_t dsc;
      lv_draw_image_dsc_init(&dsc);
      dsc.src = e.buf;
      const int64_t t0 = stat_now_us();
      lv_draw_image(layer, &dsc, a);
      s_ss_blit_us += stat_now_us() - t0;
      ++s_ss_blit_n;
      s_ss_blit_px += static_cast<uint32_t>(w) * h;
      return true;
    }
  }
  skin_cache_request(im, w, h);
  return false;
}

// キャッシュ優先の 9-slice 描画。
void skin_draw_9_cached(lv_layer_t* layer, const theme_res::SkinImg* im,
                        const lv_area_t* a) {
  if (!skin_blit_cached(layer, im, a)) {
    const int64_t t0 = stat_now_us();
    skin_draw_9(layer, im, a);
    s_ss_miss_us += stat_now_us() - t0;
    ++s_ss_miss_n;
    s_ss_miss_px += static_cast<uint32_t>(lv_area_get_width(a)) *
                    lv_area_get_height(a);
  }
}

// event user_data = part | (mode << 8)。mode 0=全面 / 1=下端の帯。
void* skin_ud(int part, int mode) {
  return reinterpret_cast<void*>(
      static_cast<uintptr_t>(part | (mode << 8)));
}

// 汎用 DRAW_MAIN コールバック: 現在 state の part 画像を 9-slice で描く。
void skin_draw_cb(lv_event_t* e) {
  if (ui::skin_dbg() >= 3) return;  // 'z' 計測: 部品スキン無し
  lv_obj_t* o = lv_event_get_target_obj(e);
  const uintptr_t ud =
      reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
  const int part = static_cast<int>(ud & 0xFF);
  const Theme& t = theme();
  const theme_res::SkinSet* ss = t.skin[part];
  if (!ss) return;
  const theme_res::SkinImg* im = ss->img[skin_state_idx(lv_obj_get_state(o))];
  if (!im) im = ss->img[0];
  if (!im) return;
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  if ((ud >> 8) == 1) {
    // mode 1: 下端の帯 (caption_line)。画像の高さぶんだけ下に張る。
    const int32_t s = im->reg[4].data
                          ? static_cast<int32_t>(im->reg[4].header.h)
                          : 4;
    a.y1 = a.y2 - (s - 1);
  }
  skin_draw_9_cached(lv_event_get_layer(e), im, &a);
}

// ラベル (kLabelTag) の文字色を再帰で塗り替える。
void skin_recolor(lv_obj_t* o, lv_color_t col) {
  if (lv_obj_get_user_data(o) == kLabelTag) {
    lv_obj_set_style_text_color(o, col, 0);
    return;
  }
  const uint32_t n = lv_obj_get_child_count(o);
  for (uint32_t i = 0; i < n; ++i) {
    skin_recolor(lv_obj_get_child(o, static_cast<int32_t>(i)), col);
  }
}

void skin_recolor_part(lv_obj_t* o, int part, bool pressed) {
  const watch::ThemeSkinPart& p = theme().skin_part[part];
  const lv_color_t col = lv_color_hex(
      pressed && (p.set & (1u << 4)) ? p.text_pressed : p.text);
  skin_recolor(o, col);
}

// pressed 追従: LVGL は押下 state を子に伝播しないので、
// PRESSED/RELEASED/PRESS_LOST でラベルの色を直接切り替える。
void skin_press_cb(lv_event_t* e) {
  lv_obj_t* o = lv_event_get_target_obj(e);
  const int part = static_cast<int>(
      reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)) & 0xFF);
  skin_recolor_part(o, part, lv_obj_has_state(o, LV_STATE_PRESSED));
}

// o に part のスキンを貼る。画像があれば true (ベクタ背景はここで消す)。
// mode は skin_draw_cb の mode。use_pad=true で manifest pad を既存
// pad に加算 (画像の飾りがコンテンツに被らない内側余白)。
// text / text_pressed 指定時は中のラベルの文字色も差し替える。
bool skin_apply_obj(lv_obj_t* o, watch::ThemeSkinPartId part, int mode,
                    bool use_pad) {
  const Theme& t = theme();
  if (!(t.skin_set & (1u << part)) || !t.skin[part]) return false;
  const theme_res::SkinSet* ss = t.skin[part];
  if (!ss->img[0]) return false;
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_outline_width(o, 0, 0);
  lv_obj_set_style_shadow_width(o, 0, 0);
  const watch::ThemeSkinPart& p = t.skin_part[part];
  if (use_pad && (p.set & (1u << 2))) {
    lv_obj_set_style_pad_left(
        o, lv_obj_get_style_pad_left(o, LV_PART_MAIN) + p.pad[0], 0);
    lv_obj_set_style_pad_top(
        o, lv_obj_get_style_pad_top(o, LV_PART_MAIN) + p.pad[1], 0);
    lv_obj_set_style_pad_right(
        o, lv_obj_get_style_pad_right(o, LV_PART_MAIN) + p.pad[2], 0);
    lv_obj_set_style_pad_bottom(
        o, lv_obj_get_style_pad_bottom(o, LV_PART_MAIN) + p.pad[3], 0);
  }
  lv_obj_add_event_cb(o, skin_draw_cb, LV_EVENT_DRAW_MAIN,
                      skin_ud(part, mode));
  if (p.set & (3u << 3)) skin_recolor_part(o, part, false);
  if (p.set & (1u << 4)) {
    lv_obj_add_event_cb(o, skin_press_cb, LV_EVENT_PRESSED,
                        skin_ud(part, 0));
    lv_obj_add_event_cb(o, skin_press_cb, LV_EVENT_RELEASED,
                        skin_ud(part, 0));
    lv_obj_add_event_cb(o, skin_press_cb, LV_EVENT_PRESS_LOST,
                        skin_ud(part, 0));
  }
  return true;
}

// lv_switch のノブ/トラックをスキン画像で描く。ジオメトリは
// lv_switch.c の計算と同じ (アニメ中の位置は内部状態なので、
// 画像は CHECKED トグルで即座に切り替わる)。
void skin_switch_draw_cb(lv_event_t* e) {
  if (ui::skin_dbg() >= 3) return;
  lv_obj_t* o = lv_event_get_target_obj(e);
  const Theme& t = theme();
  lv_layer_t* layer = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  const bool chk = (lv_obj_get_state(o) & LV_STATE_CHECKED) != 0;
  if (const theme_res::SkinSet* tr = t.skin[watch::kSkinSwitchTrack]) {
    const theme_res::SkinImg* im =
        chk && tr->img[2] ? tr->img[2] : tr->img[0];
    if (im) skin_draw_9_cached(layer, im, &a);
  }
  if (const theme_res::SkinSet* kn = t.skin[watch::kSkinSwitchKnob]) {
    const theme_res::SkinImg* im =
        chk && kn->img[2] ? kn->img[2] : kn->img[0];
    if (im) {
      const int32_t ks = lv_area_get_height(&a);
      const int32_t len = lv_area_get_width(&a) - ks;
      lv_area_t k = a;
      k.x1 += chk ? len : 0;
      k.x2 = k.x1 + ks - 1;
      k.x1 -= lv_obj_get_style_pad_left(o, LV_PART_KNOB);
      k.x2 += lv_obj_get_style_pad_right(o, LV_PART_KNOB);
      k.y1 -= lv_obj_get_style_pad_top(o, LV_PART_KNOB);
      k.y2 += lv_obj_get_style_pad_bottom(o, LV_PART_KNOB);
      skin_draw_9_cached(layer, im, &k);
    }
  }
}

// lv_slider の track/fill/knob をスキン画像で描く。ジオメトリは
// lv_bar.c draw_indic + lv_slider.c position_knob と同じ式
// (値は現在値のみ — ドラッグ中のアニメ途中値は参照しない)。
void skin_slider_draw_cb(lv_event_t* e) {
  if (ui::skin_dbg() >= 3) return;
  lv_obj_t* o = lv_event_get_target_obj(e);
  const Theme& t = theme();
  lv_layer_t* layer = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(o, &a);

  if (const theme_res::SkinSet* tr = t.skin[watch::kSkinSliderTrack]) {
    if (tr->img[0]) skin_draw_9_cached(layer, tr->img[0], &a);
  }

  // indic_area = coords − MAIN pad。
  lv_area_t ind = a;
  ind.x1 += lv_obj_get_style_pad_left(o, LV_PART_MAIN);
  ind.x2 -= lv_obj_get_style_pad_right(o, LV_PART_MAIN);
  ind.y1 += lv_obj_get_style_pad_top(o, LV_PART_MAIN);
  ind.y2 -= lv_obj_get_style_pad_bottom(o, LV_PART_MAIN);
  const int32_t mn = lv_slider_get_min_value(o);
  const int32_t mx = lv_slider_get_max_value(o);
  const int32_t rng = mx - mn;
  const int32_t pos =
      rng > 0 ? lv_area_get_width(&ind) * (lv_slider_get_value(o) - mn) / rng
              : 0;
  lv_area_t fill = ind;
  fill.x2 = ind.x1 + pos;  // lv_bar.c: *axis2 = *axis1 + anim_cur_value_x
  const bool pressed = (lv_obj_get_state(o) & LV_STATE_PRESSED) != 0;
  if (const theme_res::SkinSet* fl = t.skin[watch::kSkinSliderFill]) {
    const theme_res::SkinImg* im =
        pressed && fl->img[1] ? fl->img[1] : fl->img[0];
    // fill は値で幅が変わるのでキャッシュしない (幅ごとにエントリが
    // 増えて LRU を押し出すだけ)。直接 9-slice で描く。
    if (im && pos > 0) {
      const int64_t t0 = stat_now_us();
      skin_draw_9(layer, im, &fill);
      s_ss_miss_us += stat_now_us() - t0;
      ++s_ss_miss_n;
      s_ss_miss_px += static_cast<uint32_t>(pos) * lv_area_get_height(&fill);
    }
  }
  if (const theme_res::SkinSet* kn = t.skin[watch::kSkinSliderKnob]) {
    const theme_res::SkinImg* im =
        pressed && kn->img[1] ? kn->img[1] : kn->img[0];
    if (im) {
      // position_knob: knob_size=obj高、中心=indic_area.x2、縦はcoords。
      const int32_t ks = lv_area_get_height(&a);
      lv_area_t k;
      k.x1 = fill.x2 - (ks >> 1);
      k.x2 = k.x1 + ks - 1;
      k.y1 = a.y1;
      k.y2 = a.y2;
      k.x1 -= lv_obj_get_style_pad_left(o, LV_PART_KNOB);
      k.x2 += lv_obj_get_style_pad_right(o, LV_PART_KNOB);
      k.y1 -= lv_obj_get_style_pad_top(o, LV_PART_KNOB);
      k.y2 += lv_obj_get_style_pad_bottom(o, LV_PART_KNOB);
      skin_draw_9_cached(layer, im, &k);
    }
  }
}

}  // namespace

// theme_res::reset() から呼ぶ: SkinImg* が無効になるので全エントリ破棄。
// LVGL タスク内 (theme_apply 経路) 専用。
void skin_cache_clear() {
  for (auto& e : s_scache) {
    if (e.buf) lv_draw_buf_destroy(e.buf);
    e = SkinCacheEnt{};
  }
  s_screq_n = 0;
}

void skin_stats_reset() {
  s_ss_blit_us = s_ss_miss_us = 0;
  s_ss_blit_n = s_ss_miss_n = 0;
  s_ss_blit_px = s_ss_miss_px = 0;
  s_ss_rej_size = s_ss_rej_room = 0;
  s_ss_fill_n = s_ss_band_n = s_ss_imgreg_n = 0;
  s_ss_rej_w = s_ss_rej_h = s_ss_rej_sw = s_ss_rej_sh = 0;
  memset(s_ss_bail, 0, sizeof(s_ss_bail));
}

void skin_stats(uint64_t* blit_us, uint32_t* blit_n, uint64_t* miss_us,
                uint32_t* miss_n, uint32_t* blit_px, uint32_t* miss_px,
                uint32_t* rej_size, uint32_t* rej_room, uint32_t* fill_n,
                uint32_t* band_n, uint32_t* imgreg_n, uint32_t bail[10]) {
  *blit_us = s_ss_blit_us;
  *blit_n = s_ss_blit_n;
  *miss_us = s_ss_miss_us;
  *miss_n = s_ss_miss_n;
  *blit_px = s_ss_blit_px;
  *miss_px = s_ss_miss_px;
  *rej_size = s_ss_rej_size;
  *rej_room = s_ss_rej_room;
  *fill_n = s_ss_fill_n;
  *band_n = s_ss_band_n;
  *imgreg_n = s_ss_imgreg_n;
  bail[0] = s_ss_bail[0];
  bail[1] = s_ss_bail[1];
  bail[2] = s_ss_bail[2];
  bail[3] = s_ss_bail[3];
  bail[4] = s_ss_bail[4];
  bail[5] = s_ss_bail[5];
  bail[6] = s_ss_rej_w;
  bail[7] = s_ss_rej_h;
  bail[8] = s_ss_rej_sw;
  bail[9] = s_ss_rej_sh;
}

lv_obj_t* header(lv_obj_t* scr, const char* title, bool back_btn) {
  const Theme& t = theme();
  lv_obj_add_event_cb(scr, on_any_click, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* h = flat(scr);
  lv_obj_set_size(h, kW, kHeaderH);
  lv_obj_set_pos(h, 0, 0);
  lv_obj_set_style_pad_left(h, kPad, 0);
  // 右は少し広めに: タイトル (flex_grow=1) が 368px 内に収まり、
  // 角の安全域 (x>310 && y<100) から外れる。
  lv_obj_set_style_pad_right(h, 42, 0);
  // コンテンツ領域の中央がピル中心 ~47px になる pad。ピルの描画角が
  // パネルの丸角 (R≈100) の安全域に収まるよう上から少し下げた。
  lv_obj_set_style_pad_top(h, 26, 0);
  lv_obj_set_style_pad_bottom(h, 8, 0);
  lv_obj_set_style_pad_column(h, 8, 0);
  lv_obj_set_flex_flow(h, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(h, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  // ヘッダ帯の画像スキン (背景全面に描画。pad はレイアウトに効くので
  // ここでは加算しない)。
  skin_apply_obj(h, watch::kSkinHeaderBar, 0, false);

  const bool flat_hdr =
      (t.style.set & (1u << watch::kStyleHeader)) && t.style.header == 1;
  if (back_btn) {
    lv_obj_t* b = lv_button_create(h);
    clickable(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, kBackH);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    // style.header="flat" はピルの背景を消す (画像背景にかぶせる系)。
    lv_obj_set_style_bg_color(b, t.surface2, 0);
    if (flat_hdr) lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_left(b, 8, 0);
    lv_obj_set_style_pad_right(b, 16, 0);
    lv_obj_set_style_pad_column(b, 8, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    mk_label(b, "‹", &font_chev_22, t.text_dim);
    mk_label(b, "戻る", t.font_body, t.text_dim);
    lv_obj_add_event_cb(
        b, [](lv_event_t*) { ui::emit(watch::ActionType::Back); },
        LV_EVENT_CLICKED, nullptr);
    skin_apply_obj(b, watch::kSkinBackPill, 0, true);
  }

  lv_obj_t* tl = mk_label(h, title, t.font_title, t.text);
  // タイトルが長いときは末尾省略。DOTS は高さが可変だと折り返して
  // しまうので 1 行高に固定する。右端は pad_right=42 で角帯を避ける。
  lv_obj_set_flex_grow(tl, 1);
  lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);
  lv_obj_set_height(tl, lv_font_get_line_height(t.font_title) + 2);
  lv_obj_set_style_margin_top(tl, 7, 0);
  lv_obj_set_user_data(h, kHeaderTag);
  return h;
}

lv_obj_t* content(lv_obj_t* scr) {
  const Theme& t = theme();
  lv_obj_t* c = flat(scr);
  // スクロール領域は画面下端 (y=502) まで。丸角の角で削られるのは
  // パネル側の自然な見え方 (T27-b: 途中で切れる帯を残さない)。
  // 代わりに pad_bottom を入れて、末尾までスクロールしたとき最後の
  // 要素の下端が安全域 (502-(kPad+6)) に収まるようにする。
  lv_obj_set_size(c, kW - 2 * kPad, 502 - kHeaderH);
  lv_obj_set_pos(c, kPad, kHeaderH);
  lv_obj_set_style_pad_bottom(c, kPad + 6, 0);
  lv_obj_set_style_pad_row(c, 8, 0);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_scroll_dir(c, LV_DIR_VER);
  lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  // スクロールバーは出さない (腕時計 UI、カードの上に太く被さるため)。
  lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_OFF);
  (void)t;
  return c;
}

// ---- bake_scroll: 静的スクロール内容の画像化 ----

static void bake_free_buf(lv_event_t* e) {
  lv_draw_buf_destroy(
      static_cast<lv_draw_buf_t*>(lv_event_get_user_data(e)));
}

BakeZone bake_rect(lv_obj_t* col, lv_obj_t* row, lv_event_cb_t cb,
                   void* ud) {
  lv_area_t ca, ra;
  lv_obj_get_content_coords(col, &ca);
  lv_obj_get_coords(row, &ra);
  BakeZone z{static_cast<lv_coord_t>(ra.x1 - ca.x1),
             static_cast<lv_coord_t>(ra.y1 - ca.y1),
             static_cast<lv_coord_t>(lv_area_get_width(&ra)),
             static_cast<lv_coord_t>(lv_area_get_height(&ra)),
             cb,
             ud};
  return z;
}

bool bake_scroll(lv_obj_t* col, lv_obj_t* inner, const BakeZone* zones,
                 size_t zone_cnt) {
  lv_obj_update_layout(col);
  lv_area_t ca, ia;
  lv_obj_get_content_coords(col, &ca);
  lv_obj_get_coords(inner, &ia);

  // ARGB8888 (4B/px) + 全面クリア: lv_snapshot は RGB565A8 を
  // サポートせず、ARGB4444 は画像ソースとして描けない
  // (lv_snapshot.c / draw 側の Not supported color format)。
  // カード角丸の外側は透明のまま残す (T27-a 対策)。
  constexpr lv_color_format_t kBakeCf = LV_COLOR_FORMAT_ARGB8888;
  lv_draw_buf_t* buf = lv_snapshot_create_draw_buf(inner, kBakeCf);
  if (!buf) {
    // フォント・アリーナ常駐後の断片化 (T24) で ~850KB の連続確保に
    // 失敗しうる。スキンキャッシュ (最大 512KB) を捨てて一度だけ
    // 再試行する — キャッシュは以後のフレームで自然に再構築される。
    skin_cache_clear();
    buf = lv_snapshot_create_draw_buf(inner, kBakeCf);
  }
  if (!buf) return false;
  lv_draw_buf_clear(buf, nullptr);
  if (lv_snapshot_take_to_draw_buf(inner, kBakeCf, buf) !=
      LV_RESULT_OK) {
    lv_draw_buf_destroy(buf);
    return false;
  }

  lv_obj_clean(col);

  lv_obj_t* im = lv_image_create(col);
  lv_obj_add_flag(im, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_image_set_src(im, reinterpret_cast<const lv_image_dsc_t*>(buf));
  lv_obj_set_pos(im, ia.x1 - ca.x1, ia.y1 - ca.y1);
  lv_obj_add_event_cb(im, bake_free_buf, LV_EVENT_DELETE, buf);

  // 行位置に透明クリックゾーンを復元。
  for (size_t i = 0; i < zone_cnt; i++) {
    const BakeZone& z = zones[i];
    lv_obj_t* t = flat(col);
    lv_obj_add_flag(t, static_cast<lv_obj_flag_t>(
                           LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_IGNORE_LAYOUT));
    lv_obj_set_pos(t, z.x, z.y);
    lv_obj_set_size(t, z.w, z.h);
    lv_obj_add_event_cb(t, z.cb, LV_EVENT_CLICKED, z.ud);
  }
  return true;
}

namespace {

lv_obj_t* mk_button(lv_obj_t* parent, const char* text,
                    lv_event_cb_t cb, void* ud, lv_color_t bg,
                    lv_opa_t bg_opa, lv_color_t fg,
                    watch::ThemeSkinPartId part) {
  const Theme& t = theme();
  lv_obj_t* b = lv_button_create(parent);
  clickable(b);
  lv_obj_set_size(b, LV_PCT(100), kBtnH);
  // radius は高さの半分まで (小さいボタンで突き抜けない)。
  lv_obj_set_style_radius(b, LV_MIN(skin_btn_radius(), kBtnH / 2), 0);
  lv_obj_set_style_bg_color(b, bg, 0);
  lv_obj_set_style_bg_opa(b, bg_opa, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_t* l = mk_label(b, text, t.font_body, fg);
  // 長いラベルは末尾 … にする (幅をボタン幅に固定しないと省略されない)。
  lv_obj_set_width(l, LV_PCT(100));
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  // DOTS は高さが可変だと2行目が覗くので 1 行高に固定。
  lv_obj_set_height(l, lv_font_get_line_height(t.font_body) + 2);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(l);
  skin_apply_obj(b, part, 0, true);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  return b;
}

}  // namespace

lv_obj_t* button(lv_obj_t* parent, const char* text, lv_event_cb_t cb,
                 void* ud) {
  const Theme& t = theme();
  return mk_button(parent, text, cb, ud, t.surface2, LV_OPA_COVER, t.text,
                   watch::kSkinBtnSecondary);
}

lv_obj_t* button_primary(lv_obj_t* parent, const char* text, lv_event_cb_t cb,
                        void* ud) {
  const Theme& t = theme();
  return mk_button(parent, text, cb, ud, t.primary, LV_OPA_COVER,
                   t.on_primary, watch::kSkinBtnPrimary);
}

lv_obj_t* button_danger(lv_obj_t* parent, const char* text, lv_event_cb_t cb,
                       void* ud) {
  const Theme& t = theme();
  // モックの rgba(danger, .16)。
  return mk_button(parent, text, cb, ud, t.danger, 41, t.danger,
                   watch::kSkinBtnDanger);
}

lv_obj_t* group(lv_obj_t* parent) {
  lv_obj_t* g = lv_obj_create(parent);
  bubble(g);
  lv_obj_set_size(g, LV_PCT(100), LV_SIZE_CONTENT);
  skin_card(g);
  lv_obj_set_style_pad_all(g, 0, 0);
  lv_obj_set_style_pad_row(g, 0, 0);  // pad_all では行間はゼロにならない
  lv_obj_set_flex_flow(g, LV_FLEX_FLOW_COLUMN);
  lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_user_data(g, kGroupTag);
  skin_apply_obj(g, watch::kSkinListGroup, 0, true);
  return g;
}

lv_obj_t* card(lv_obj_t* parent) {
  const Theme& t = theme();
  lv_obj_t* c = lv_obj_create(parent);
  bubble(c);
  lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
  skin_card(c);
  lv_obj_set_style_pad_all(c, 16, 0);
  lv_obj_set_style_pad_row(c, t.space, 0);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_START);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  skin_apply_obj(c, watch::kSkinCard, 0, true);
  return c;
}

lv_obj_t* row_box(lv_obj_t* grp, lv_event_cb_t cb, void* ud) {
  divider_if_needed(grp);
  lv_obj_t* r = flat(grp);
  if (cb) clickable(r);
  lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_min_height(r, kRowMinH, 0);
  lv_obj_set_style_pad_left(r, kRowPadX, 0);
  lv_obj_set_style_pad_right(r, kRowPadX, 0);
  lv_obj_set_style_pad_column(r, kRowGap, 0);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  skin_apply_obj(r, watch::kSkinRow, 0, true);
  if (cb) lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, ud);
  return r;
}

lv_obj_t* row_text(lv_obj_t* row, const char* text, const char* sub,
                   int32_t max_w) {
  const Theme& t = theme();
  lv_obj_t* cell = flat(row);
  lv_obj_set_size(cell, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_min_width(cell, 60, 0);  // flex_grow と組で右要素を右端へ
  // タイトルとサブは各行の行高 (フォントの leading 含む) で自然に
  // 間が空く。負の pad で重ねるとグリフの ink がぶつかる (T16)。
  lv_obj_set_style_pad_row(cell, 0, 0);
  // 行が SIZE_CONTENT だと LVGL は track_place を START に固定し、
  // 子が行の上寄りに張り付く。セルに行の最低高さを持たせて、セル内で
  // タイトル (+サブ) ブロックを垂直中央揃えにする。
  lv_obj_set_style_min_height(cell, kRowMinH, 0);
  lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_flex_grow(cell, 1);

  // PCT(100) はセル自体が SIZE_CONTENT のので測定時に潰れて縦に巻く。
  // 行内の残り幅を呼び側から計算して固定幅 + DOT にする。
  lv_obj_t* l = mk_label(cell, text, t.font_row, t.text);
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  lv_obj_set_width(l, max_w);
  if (sub) {
    lv_obj_t* s = mk_label(cell, sub, t.font_body, t.text_dim);
    lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s, max_w);
  }
  return cell;
}

namespace {

// 行内容幅 = 366 - 左右pad 16*2。タイトルセルの残り予算を引く。
int32_t text_budget(bool icon, bool chev, bool value) {
  int32_t w = 334;
  if (icon) w -= kIconTile + kRowGap;
  if (chev) w -= 22 + kRowGap;
  if (value) w -= 100;  // 値テキストの予約分 (短い値想定)
  return w < 60 ? 60 : w;
}

}  // namespace

lv_obj_t* row_value(lv_obj_t* row, const char* value) {
  const Theme& t = theme();
  return mk_label(row, value, t.font_body, t.text_dim);
}

lv_obj_t* row(lv_obj_t* grp, const char* text, const char* sub,
              const char* value, bool chev, lv_event_cb_t cb, void* ud) {
  lv_obj_t* r = row_box(grp, cb, ud);
  row_text(r, text, sub, text_budget(false, chev, value != nullptr));
  if (value) row_value(r, value);
  if (chev) chevron(r);
  return r;
}

lv_obj_t* back_row(lv_obj_t* grp, lv_event_cb_t cb) {
  const Theme& t = theme();
  lv_obj_t* r = row_box(grp, cb, nullptr);
  mk_label(r, "‹", &font_chev_22, t.text_dim);
  row_text(r, "戻る", nullptr, text_budget(false, false, false));
  return r;
}

lv_obj_t* row_icon(lv_obj_t* grp, const char* icon, lv_color_t icon_bg,
                   const char* text, const char* sub, bool chev,
                   lv_event_cb_t cb, void* ud) {
  return row_icon_img(grp, nullptr, icon, icon_bg, text, sub, chev, cb,
                      ud);
}

lv_obj_t* row_icon_img(lv_obj_t* grp, const lv_image_dsc_t* imgd,
                       const char* icon, lv_color_t icon_bg,
                       const char* text, const char* sub, bool chev,
                       lv_event_cb_t cb, void* ud) {
  const Theme& t = theme();
  lv_obj_t* r = row_box(grp, cb, ud);
  if (imgd) {
    // テーマのアイコン画像 (タイルを丸ごと置き換え)
    lv_obj_t* im = lv_image_create(r);
    lv_obj_set_size(im, kIconTile, kIconTile);
    lv_obj_set_style_radius(im, 11, 0);
    lv_obj_set_style_clip_corner(im, true, 0);
    lv_image_set_src(im, imgd);
    lv_obj_add_flag(im, LV_OBJ_FLAG_EVENT_BUBBLE);
  } else if (icon) {
    lv_obj_t* ic = flat(r);
    lv_obj_set_size(ic, kIconTile, kIconTile);
    lv_obj_set_style_radius(ic, 11, 0);
    // 薄い色付きタイル + primary のアイコンフォント (T7)。
    // icon が PUA (U+E000 以降) なら Material Symbols グリフ、
    // それ以外は従来どおり本文フォントの文字として描く。
    lv_obj_set_style_bg_opa(ic, LV_OPA_40, 0);
    lv_obj_set_style_bg_color(ic, icon_bg, 0);
    const bool glyph =
        (reinterpret_cast<const unsigned char*>(icon)[0] & 0xFE) == 0xEE;
    lv_obj_t* il = mk_label(ic, icon, glyph ? &font_icon_36 : t.font_body,
                            glyph ? t.primary
                                  : lv_color_hex(0x000000));
    lv_obj_center(il);
    skin_apply_obj(ic, watch::kSkinIconTile, 0, false);
  }
  const bool has_icon = imgd || icon;
  row_text(r, text, sub, text_budget(has_icon, chev, false));
  if (chev) chevron(r);
  return r;
}

lv_obj_t* mk_switch(lv_obj_t* row, bool on) {
  const Theme& t = theme();
  lv_obj_t* sw = lv_switch_create(row);
  // スイッチ自身はクリックを取らない (行タップでトグル → 二重発火防止)。
  bubble(sw);
  lv_obj_remove_flag(sw, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(sw, kSwitchW, kSwitchH);
  lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  // OFF: トラック≈#3A3E4A (edge×line)、ノブ≈#AAB8BB。ON: 塗り primary + 白ノブ。
  // LVGL 既定テーマは CHECKED の INDICATOR を青にするので同じセレクタで上書き
  // (汎用セレクタだけだと ON が青のまま・OFF のノブ脇に橙が滲む)。
  lv_obj_set_style_bg_color(sw, lv_color_mix(t.edge, t.line, 128),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
  // OFF 時は INDICATOR もトラック色にして左端の滲みを消す (LVGL は
  // OFF でもインジケータを少し描く)。ON だけ CHECKED セレクタで primary。
  lv_obj_set_style_bg_color(sw, lv_color_mix(t.edge, t.line, 128),
                            LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sw, t.primary,
                            LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_bg_opa(sw, LV_OPA_COVER,
                          LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(
      sw, lv_color_mix(t.text_dim, t.text, 170), LV_PART_KNOB);
  lv_obj_set_style_bg_color(sw, lv_color_hex(0xFFFFFF),
                            LV_PART_KNOB | LV_STATE_CHECKED);
  // LVGL の switch は KNOB pad 分だけノブが track より大きくなる
  // (pad<0 で小さく)。knob = track - 2·3 = 24 にするため pad=-3。
  lv_obj_set_style_pad_all(sw, (kKnob - kSwitchH) / 2, LV_PART_KNOB);
  // ノブの影・外周は付けない (ON/OFF は色だけで表す)。
  lv_obj_set_style_shadow_width(sw, 0, LV_PART_KNOB);
  lv_obj_set_style_outline_width(sw, 0, LV_PART_KNOB);
  const bool sk_tr = theme_skin(watch::kSkinSwitchTrack) != nullptr;
  const bool sk_kn = theme_skin(watch::kSkinSwitchKnob) != nullptr;
  if (sk_tr) {
    lv_obj_set_style_bg_opa(sw, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sw, LV_OPA_TRANSP,
                            LV_PART_INDICATOR | LV_STATE_CHECKED);
  }
  if (sk_kn) {
    lv_obj_set_style_bg_opa(sw, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, LV_OPA_TRANSP,
                            LV_PART_KNOB | LV_STATE_CHECKED);
  }
  if (sk_tr || sk_kn) {
    // ノブ画像のはみ出し分は KNOB pad が ext_draw_size に計上済み。
    lv_obj_add_event_cb(sw, skin_switch_draw_cb, LV_EVENT_DRAW_MAIN,
                        nullptr);
  }
  if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
  return sw;
}

lv_obj_t* row_switch(lv_obj_t* grp, const char* text, const char* sub,
                     bool on, lv_event_cb_t cb, void* ud) {
  lv_obj_t* r = row_box(grp, cb, ud);
  row_text(r, text, sub, text_budget(false, false, true));
  mk_switch(r, on);
  return r;
}

lv_obj_t* switch_of(lv_obj_t* row) {
  // 最後の子がスイッチ (row_text のセルと仕切りを除く)。
  const uint32_t n = lv_obj_get_child_count(row);
  return n ? lv_obj_get_child(row, static_cast<int32_t>(n) - 1) : nullptr;
}

lv_obj_t* caption(lv_obj_t* parent, const char* text) {
  const Theme& t = theme();
  lv_obj_t* l = mk_label(parent, text, t.font_body, t.text_dim);
  // content 列は子を水平センタリングするので、幅いっぱいにして左揃え。
  lv_obj_set_width(l, LV_PCT(100));
  lv_obj_set_style_text_letter_space(l, 2, 0);
  lv_obj_set_style_pad_left(l, 6, 0);
  lv_obj_set_style_pad_bottom(l, 0, 0);
  // v3 skin: caption_line はラベル下端の帯として描く。
  if (theme_skin(watch::kSkinCaptionLine)) {
    lv_obj_set_style_pad_bottom(l, 6, 0);
    skin_apply_obj(l, watch::kSkinCaptionLine, 1, false);
  }
  return l;
}

namespace {

// 値表示の更新 (スライダーの VALUE_CHANGED で先に呼ぶ)。user_data = 値ラベル。
void slider_upd_label(lv_obj_t* s, lv_obj_t* vl) {
  const char* suf =
      static_cast<const char*>(lv_obj_get_user_data(s));
  lv_label_set_text_fmt(vl, "%d%s", static_cast<int>(lv_slider_get_value(s)),
                        suf ? suf : "");
}

void on_slider_value(lv_event_t* e) {
  slider_upd_label(lv_event_get_target_obj(e),
                   static_cast<lv_obj_t*>(lv_event_get_user_data(e)));
}

lv_obj_t* slider_inner(lv_obj_t* grp, const char* label, const char* suffix,
                       int32_t min, int32_t max, int32_t value,
                       lv_event_cb_t cb, void* ud) {
  const Theme& t = theme();
  divider_if_needed(grp);
  lv_obj_t* inner = flat(grp);
  lv_obj_set_size(inner, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_pad_top(inner, 14, 0);
  lv_obj_set_style_pad_bottom(inner, 16, 0);
  lv_obj_set_style_pad_left(inner, kRowPadX, 0);
  lv_obj_set_style_pad_right(inner, kRowPadX, 0);
  lv_obj_set_style_pad_row(inner, 12, 0);
  lv_obj_set_flex_flow(inner, LV_FLEX_FLOW_COLUMN);

  lv_obj_t* top = flat(inner);
  lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  mk_label(top, label, t.font_body, t.text);
  lv_obj_t* vl = mk_label(top, "", t.font_body, t.text);

  lv_obj_t* s = lv_slider_create(inner);
  clickable(s);
  lv_obj_set_width(s, LV_PCT(100));
  lv_obj_set_height(s, kTrackH);
  lv_slider_set_range(s, min, max);
  // MAIN pad は LVGL がトラック (indic_area) と値マッピングの両方を
  // 内側にずらすのに使う (lv_bar.c)。ノブ半径分を左右に入れて、
  // 最小/最大でノブがトラック端より外に切れるのを防ぐ。
  lv_obj_set_style_pad_left(s, kSliderKnob / 2, LV_PART_MAIN);
  lv_obj_set_style_pad_right(s, kSliderKnob / 2, LV_PART_MAIN);
  lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s, t.line, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
  // 塗りは単色 (T5: グラデーションは描画負荷対策で廃止)。
  lv_obj_set_style_bg_color(s, t.primary, LV_PART_INDICATOR);
  // 白いノブ + primary の外周リング (モックの box-shadow 相当)。
  lv_obj_set_style_bg_color(s, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
  lv_obj_set_style_pad_all(s, (kSliderKnob - kTrackH) / 2, LV_PART_KNOB);
  // ノブの外周リング (outline 4) は小さいサイズで潰れるので廃止。
  // ノブの影も付けない (ON/OFF・値は色だけで表す)。
  lv_obj_set_style_outline_width(s, 0, LV_PART_KNOB);
  lv_obj_set_style_shadow_width(s, 0, LV_PART_KNOB);
  // タップ判定: 高さ 10 のトラックだけだと細いので 48px 確保 (tap_min)。
  lv_obj_set_ext_click_area(s, (48 - kTrackH) / 2);
  // v3 skin: トラック/塗り/ノブ画像があればベクタ部品を透明化して画像を描く。
  const bool sk_tr = theme_skin(watch::kSkinSliderTrack) != nullptr;
  const bool sk_fl = theme_skin(watch::kSkinSliderFill) != nullptr;
  const bool sk_kn = theme_skin(watch::kSkinSliderKnob) != nullptr;
  if (sk_tr) lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, LV_PART_MAIN);
  if (sk_fl) lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, LV_PART_INDICATOR);
  if (sk_kn) {
    lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_outline_opa(s, LV_OPA_TRANSP, LV_PART_KNOB);
  }
  if (sk_tr || sk_fl || sk_kn) {
    // ノブ画像のはみ出し分は KNOB pad が ext_draw_size に計上済み。
    lv_obj_add_event_cb(s, skin_slider_draw_cb, LV_EVENT_DRAW_MAIN,
                        nullptr);
  }
  lv_obj_set_user_data(s, const_cast<char*>(suffix));
  // 値ラベル更新 → ユーザの cb の順で呼ばれる。
  // 注意: LV_ANIM_OFF の set_value は VALUE_CHANGED を投げない (ドラッグ時のみ)
  // ので初期値はここで直接ラベルに書く。
  lv_obj_add_event_cb(s, on_slider_value, LV_EVENT_VALUE_CHANGED, vl);
  lv_slider_set_value(s, value, LV_ANIM_OFF);
  slider_upd_label(s, vl);
  if (cb) lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, ud);
  return inner;
}

}  // namespace

lv_obj_t* slider_row(lv_obj_t* parent, const char* label, const char* suffix,
                     int32_t min, int32_t max, int32_t value,
                     lv_event_cb_t cb, void* ud) {
  // 親が group() ならその中にブロックを入れる。違うならグループを作る。
  lv_obj_t* grp = (lv_obj_get_user_data(parent) == kGroupTag)
                      ? parent
                      : group(parent);
  return slider_inner(grp, label, suffix, min, max, value, cb, ud);
}

lv_obj_t* slider_of(lv_obj_t* row) {
  // slider_row が返す inner の子: 0=上段 (ラベル+値), 1=スライダー。
  return lv_obj_get_child(row, 1);
}

namespace {

lv_obj_t* slider_value_label(lv_obj_t* row) {
  // inner の子 0=上段。その子 0=ラベル, 1=値。
  lv_obj_t* top = lv_obj_get_child(row, 0);
  return top ? lv_obj_get_child(top, 1) : nullptr;
}

}  // namespace

void slider_set(lv_obj_t* row, int32_t value) {
  lv_obj_t* s = slider_of(row);
  lv_slider_set_value(s, value, LV_ANIM_OFF);
  lv_obj_t* vl = slider_value_label(row);
  if (vl) slider_upd_label(s, vl);
}

lv_obj_t* line(lv_obj_t* parent, const char* text) {
  const Theme& t = theme();
  return mk_label(parent, text, t.font_body, t.text_dim);
}

lv_obj_t* status_dot(lv_obj_t* parent, lv_color_t col) {
  lv_obj_t* d = lv_obj_create(parent);
  bubble(d);
  lv_obj_set_size(d, 14, 14);
  lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(d, col, 0);
  lv_obj_set_style_border_width(d, 0, 0);
  lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE);
  return d;
}

void set_dot(lv_obj_t* dot, lv_color_t col) {
  lv_obj_set_style_bg_color(dot, col, 0);
}

// ---- 画面隅マスコット (manifest "mascot") ----
namespace {

// 1画面1個想定だが Home が常駐するので最大2つ。静的プール (削除時に解放)。
struct MascotSt {
  lv_obj_t* img;    // 表情画像
  lv_obj_t* bub;    // セリフ吹き出し
  lv_obj_t* lbl;    // 吹き出し内テキスト
  lv_timer_t* hide; // 自動で隠すタイマー
  uint8_t expr;     // 現在の表情 index
  bool small;       // ヘッダ画面の小型表示か
  bool used;
};
MascotSt s_ms[2] = {};

// ヘッダ画面での小型マスコットの高さ (タイトルと同じ高さに収める)。
constexpr int32_t kMascotSmall = 44;

void mascot_hide(MascotSt* st) {
  if (st->bub) lv_obj_add_flag(st->bub, LV_OBJ_FLAG_HIDDEN);
  if (st->hide) {
    lv_timer_delete(st->hide);
    st->hide = nullptr;
  }
}

void mascot_hide_cb(lv_timer_t* tm) {
  mascot_hide(static_cast<MascotSt*>(lv_timer_get_user_data(tm)));
}

void mascot_tap(lv_event_t* e) {
  MascotSt* st =
      static_cast<MascotSt*>(lv_event_get_user_data(e));
  const Theme& t = theme();
  // 表情を順送り (expr_count=1 なら固定)。
  if (t.mascot.expr_count > 1) {
    st->expr = static_cast<uint8_t>((st->expr + 1) % t.mascot.expr_count);
    if (t.mascot_dsc[st->expr]) {
      lv_image_set_src(st->img, t.mascot_dsc[st->expr]);
    }
  }
  // セリフはランダム (同じの連続は避ける)。
  if (t.mascot.line_count > 0) {
    const uint32_t i = lv_rand(0, t.mascot.line_count - 1);
    lv_label_set_text(st->lbl, t.mascot.line[i]);
    lv_obj_remove_flag(st->bub, LV_OBJ_FLAG_HIDDEN);
    if (st->small) {
      // 小型表示: ヘッダ直下の右上にトースト。コンテンツ上端を塞がない。
      lv_obj_align(st->bub, LV_ALIGN_TOP_RIGHT, -22, kHeaderH + 6);
    } else {
      // 隅の大型表示: 吹き出しは画像の直上 (OUT_TOP で吸着)。
      lv_obj_align_to(st->bub, st->img, LV_ALIGN_OUT_TOP_MID, 0, -8);
    }
    if (st->hide) lv_timer_delete(st->hide);
    st->hide = lv_timer_create(mascot_hide_cb, 3000, st);
    lv_timer_set_repeat_count(st->hide, 1);
  }
}

void mascot_free(lv_event_t* e) {
  MascotSt* st =
      static_cast<MascotSt*>(lv_event_get_user_data(e));
  if (st->hide) lv_timer_delete(st->hide);
  st->used = false;
}

}  // namespace

lv_obj_t* mascot(lv_obj_t* scr, int screen_index) {
  const Theme& t = theme();
  if (!ui::theme_mascot_on(screen_index) || !t.mascot_dsc[0]) {
    return nullptr;
  }
  MascotSt* st = nullptr;
  for (auto& s : s_ms) {
    if (!s.used) {
      st = &s;
      break;
    }
  }
  if (!st) return nullptr;
  st->used = true;
  st->hide = nullptr;
  st->expr = 0;

  // ヘッダがある画面 (= リスト系) はマスコットをヘッダ右側に小型表示し、
  // コンテンツとは一切重ならないようにする。ヘッダの無い画面
  // (face/alert などレイアウトが場所を確保するもの) だけ原寸で隅に置く。
  bool has_header = false;
  const uint32_t nch = lv_obj_get_child_count(scr);
  for (uint32_t i = 0; i < nch; ++i) {
    if (lv_obj_get_user_data(lv_obj_get_child(scr,
                                            static_cast<int32_t>(i))) ==
        kHeaderTag) {
      has_header = true;
      break;
    }
  }
  st->small = has_header;

  const int32_t w = t.mascot_dsc[0]->header.w;
  const int32_t h = t.mascot_dsc[0]->header.h;
  lv_obj_t* im = lv_image_create(scr);
  lv_image_set_src(im, t.mascot_dsc[0]);
  int32_t x, y;
  if (has_header) {
    // 右端から 36px 以上・タイトルと同じ高さ (ピル中心 ~y41)。
    lv_obj_set_size(im, kMascotSmall, kMascotSmall);
    lv_image_set_scale(im, static_cast<int32_t>(
                               kMascotSmall * LV_SCALE_NONE / w));
    x = kW - 36 - kMascotSmall;
    y = 41 - kMascotSmall / 2 + 6;  // 角クリップ圏 (実測R≈100) を避けて少し下へ
  } else {
    // x/y<0 は右/下端基準のオフセット。
    x = t.mascot.x < 0 ? kW + t.mascot.x - w : t.mascot.x;
    y = t.mascot.y < 0 ? 502 + t.mascot.y - h : t.mascot.y;
    // 丸角 (実測 R≈100px) の内側の安全域に収める: 隅要素は端から
    // 36px 以内は使わない (R(1-1/√2)≈29px + 余裕)。
    if (x < 36) x = 36;
    if (x > kW - 36 - w) x = kW - 36 - w;
    if (y < 36) y = 36;
    if (y > 502 - 36 - h) y = 502 - 36 - h;
  }
  lv_obj_set_pos(im, x, y);
  clickable(im);
  lv_obj_add_event_cb(im, mascot_tap, LV_EVENT_CLICKED, st);
  // 画面が消えるときプールを解放 (タイマーが残って DANGLING しないよう)。
  lv_obj_add_event_cb(im, mascot_free, LV_EVENT_DELETE, st);
  st->img = im;

  // 吹き出し (最初は非表示)。上に張り付くので画面座標で置く。
  lv_obj_t* bub = flat(scr);
  lv_obj_set_size(bub, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_max_width(bub, 190, 0);
  lv_obj_set_style_radius(bub, 14, 0);
  lv_obj_set_style_bg_color(bub, t.surface2, 0);
  lv_obj_set_style_bg_opa(bub, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_left(bub, 10, 0);
  lv_obj_set_style_pad_right(bub, 10, 0);
  lv_obj_set_style_pad_top(bub, 6, 0);
  lv_obj_set_style_pad_bottom(bub, 6, 0);
  lv_obj_t* lbl = mk_label(bub, "", t.font_body, t.text);
  lv_obj_set_width(lbl, 170);
  // 吹き出し自身はタップを取らない (出ている間に下の操作を塞がない)。
  lv_obj_remove_flag(bub, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(bub, LV_OBJ_FLAG_HIDDEN);
  skin_apply_obj(bub, watch::kSkinToast, 0, true);
  st->bub = bub;
  st->lbl = lbl;
  return im;
}

bool skin_obj(lv_obj_t* o, watch::ThemeSkinPartId part) {
  return skin_apply_obj(o, part, 0, true);
}


// ---- 確認ダイアログ (T14) -------------------------------------------------

namespace {
struct CfmCtx {
  lv_event_cb_t cb = nullptr;
  void* ud = nullptr;
  lv_obj_t* dlg = nullptr;
};
CfmCtx s_cfm;

void cfm_close() {
  if (s_cfm.dlg) {
    lv_obj_delete(s_cfm.dlg);
    s_cfm.dlg = nullptr;
  }
  s_cfm.cb = nullptr;
  s_cfm.ud = nullptr;
}

void cfm_no(lv_event_t*) { cfm_close(); }

}  // namespace

void confirm(lv_obj_t* /*parent*/, const char* msg, lv_event_cb_t cb,
             void* ud) {
  const Theme& t = theme();
  cfm_close();  // 既出なら置き換え

  lv_obj_t* dlg = lv_obj_create(lv_layer_top());
  s_cfm.dlg = dlg;
  lv_obj_remove_flag(dlg, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(dlg, 410, 502);
  lv_obj_set_pos(dlg, 0, 0);
  lv_obj_set_style_pad_all(dlg, 0, 0);
  lv_obj_set_style_bg_color(dlg, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(dlg, LV_OPA_60, 0);
  lv_obj_set_style_border_width(dlg, 0, 0);
  lv_obj_add_flag(dlg, LV_OBJ_FLAG_CLICKABLE);
  // 枠外タップで閉じる。
  lv_obj_add_event_cb(dlg, cfm_no, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* box = card(dlg);
  lv_obj_set_size(box, 320, LV_SIZE_CONTENT);
  lv_obj_center(box);
  lv_obj_add_flag(box, LV_OBJ_FLAG_EVENT_BUBBLE);

  lv_obj_t* l = mk_label(box, msg, t.font_body, t.text);
  lv_obj_set_width(l, LV_PCT(100));

  // 「はい」は呼び出し元の cb+ud をそのまま動かし、その後に閉じる。
  lv_obj_t* yes = button_primary(box, "はい", cb, ud);
  lv_obj_add_event_cb(yes, cfm_no, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* no = button(box, "いいえ", cfm_no, nullptr);
  (void)no;
}

}  // namespace ui::c
