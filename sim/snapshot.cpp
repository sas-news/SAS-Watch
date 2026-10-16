// snapshot.cpp — ホストシミュレータ & スクリーンショット出力。
//   sim/build/snapshot --out docs/screenshots/
//   LVGL のメモリ上ディスプレイ 410x502 に ui コンポーネントを描画し、
//   各画面を PNG に書き出す。実機と同じ ui::create + EventBus + Action の
//   パイプラインを通す (タップも lv_indev 経由で本物のイベントとして打つ)。
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>
#include <cstring>
#include <filesystem>
#include <string>

#include "lvgl.h"
#include "png.hpp"
#include "sim_platform.hpp"
#include "ui/face_data.hpp"
#include "ui/port.hpp"
#include "ui/ui.hpp"
#include "watch/features/alarm.hpp"
#include "watch/features/media.hpp"
#include "watch/features/agent.hpp"
#include "watch/features/memo.hpp"
#include "watch/features/notify.hpp"
#include "watch/features/steps.hpp"
#include "watch/input_mapper.hpp"
#include "watch/safe_area.hpp"
#include "watch/power.hpp"
#include "watch/runtime.hpp"
#include "components.hpp"
#include "theme.hpp"

namespace {

constexpr int kW = 410;
constexpr int kH = 502;

// LVGL draw buffer (partial render) → flush で全画面フレームへコピー。
uint16_t s_draw[kW * 60];
uint16_t s_fb[kW * kH];
lv_display_t* s_disp = nullptr;
lv_indev_t* s_indev = nullptr;

// core 周り (firmware の watch_app と同じ組み立て)。
watch::EventBus s_bus;
watch::Navigator s_nav(&s_bus);
watch::PowerPolicy s_power(&s_bus);
watch::InputMapper s_input;
watch::FeatureRegistry s_features(watch::builtin_features(),
                                  watch::builtin_features_count());
watch::Settings s_settings;
watch::Runtime s_rt(s_bus, s_nav, s_power, s_input, s_features, s_settings);
watch::FeatureContext* s_fctx = nullptr;

struct Ptr {
  int16_t x = 0, y = 0;
  bool pressed = false;
} s_ptr;

void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
  // RGB565 little-endian。行ごとにコピー。
  const int w = area->x2 - area->x1 + 1;
  for (int y = area->y1; y <= area->y2; ++y) {
    std::memcpy(&s_fb[y * kW + area->x1],
                px_map + (y - area->y1) * w * 2, w * 2);
  }
  lv_display_flush_ready(disp);
}

void read_cb(lv_indev_t*, lv_indev_data_t* d) {
  d->point.x = s_ptr.x;
  d->point.y = s_ptr.y;
  d->state = s_ptr.pressed ? LV_INDEV_STATE_PRESSED
                           : LV_INDEV_STATE_RELEASED;
}

void sink(const watch::Action& a) { s_rt.queue().push(a); }

void pump(int ms) {
  const int steps = (ms + 29) / 30;
  for (int i = 0; i < steps; ++i) {
    sim::clock().advance_ms(30);
    // app タスク相当: キューを1件処理 + tick + power。
    s_rt.step(sim::clock().now_ms(), *s_fctx);
    lv_timer_handler();
  }
}

// ---- 安全域監査 (roadmap T0-2) ---------------------------------------
// 各スクリーンショット取得時に LVGL オブジェクトツリーを巡回し、
// テキストを持つか CLICKABLE なオブジェクトの座標を safe_area で判定。
// 装飾 (画像・背景) は対象外、画面外のオブジェクトも角カットとは無関係
// なので無視する。違反は <out>/safe_area_report.txt に書く。

FILE* s_report = nullptr;
const char* s_audit_screen = nullptr;
int s_report_lines = 0;  // 違反があれば exit code にも出す

void report_line(const char* fmt, ...) {
  if (!s_report) return;
  ++s_report_lines;
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(s_report, fmt, ap);
  va_end(ap);
}

void audit_walk(lv_obj_t* o, const lv_area_t& clip, bool in_scroll) {
  if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  // 画面と交差するものだけ (スクロールで外に出た行は角カットと無関係)。
  if (a.x1 < kW && a.x2 >= 0 && a.y1 < kH && a.y2 >= 0) {
    // 全画面をほぼ覆うもの (画面root・アラート背景) は全幅の帯として除外。
    const int w = a.x2 - a.x1 + 1, h = a.y2 - a.y1 + 1;
    const bool full_bleed = (a.x1 <= 0 && a.x2 >= kW - 1);
    if (!full_bleed) {
      // 種類名は public API の lv_obj_check_type で判別 (class 構造体は private)。
      const struct {
        const lv_obj_class_t* cls;
        const char* name;
      } kinds[] = {
          {&lv_label_class, "label"}, {&lv_button_class, "button"},
          {&lv_arc_class, "arc"},     {&lv_slider_class, "slider"},
          {&lv_switch_class, "sw"},   {&lv_image_class, "img"},
      };
      const char* kind = "obj";
      const char* text = nullptr;
      for (const auto& k : kinds) {
        if (lv_obj_check_type(o, k.cls)) {
          kind = k.name;
          if (k.cls == &lv_label_class) text = lv_label_get_text(o);
          break;
        }
      }
      // 対象: テキストを持つ、または自分で描く要素 (bg/border/角丸が見える)。
      // 透明なクリックゾーンやレイアウト容器は対象外。
      const int32_t radius = lv_obj_get_style_radius(o, LV_PART_MAIN);
      const bool draws =
          lv_obj_get_style_bg_opa(o, LV_PART_MAIN) > LV_OPA_TRANSP ||
          lv_obj_get_style_border_width(o, LV_PART_MAIN) > 0;
      const bool check = (text && text[0]) ||
                         (lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE) && draws) ||
                         (radius > 0 && draws);
      if (check) {
        // 角丸オブジェクトは描画角がバウンディング角より内側
        // (内側の弧の極値 = r·(1-√2/2) ≈ 0.293·r)。
        const int r = LV_MIN(LV_MIN(radius, w / 2), h / 2);
        const int k = (int)(r * 0.293f + 0.5f);
        const int cx[4] = {a.x1 + k, a.x2 - k, a.x1 + k, a.x2 - k};
        const int cy[4] = {a.y1 + k, a.y1 + k, a.y2 - k, a.y2 - k};
        bool bad = false;
        // スクロール領域の内側にある要素は途中位置が一時的なので
        // 角カット判定の対象外 (下端まで伸びた viewport ではスクロール
        // 中の行がパネル死角を自然に通過するだけ)。静止状態の安全は
        // content() の pad_bottom が構造的に保証する。
        if (!in_scroll) {
          for (int i = 0; i < 4; ++i) {
            // 親の clip 内・画面内にある角点だけ判定
            // (clip 外の角点は描かれないので角カットに無関係)。
            const bool in_clip = cx[i] >= clip.x1 && cx[i] <= clip.x2 &&
                                 cy[i] >= clip.y1 && cy[i] <= clip.y2;
            if (in_clip && cx[i] >= 0 && cx[i] < kW && cy[i] >= 0 &&
                cy[i] < kH && !watch::point_in_safe_area(cx[i], cy[i])) {
              bad = true;
            }
          }
        }
        if (bad) {
          char head[40] = "";
          if (text) {
            std::snprintf(head, sizeof(head), "%.30s", text);
            for (char* q = head; *q; ++q)
              if (*q == '\n') *q = ' ';
          }
          report_line("%s / %s / (%d,%d)-(%d,%d) / \"%s\"\n",
                      s_audit_screen, kind, (int)a.x1, (int)a.y1,
                      (int)a.x2, (int)a.y2, head);
        }
        // 左右余白: テキスト bbox がコンテンツ余白 kPad(32) を越えたら警告。
        // スクロール領域の内側 (clip が画面全体でない) は行幅に制限済み
        // なので除外し、自由配置の面だけを見る。
        const bool free_pos =
            clip.x1 <= 0 && clip.y1 <= 0 && clip.x2 >= kW - 1 &&
            clip.y2 >= kH - 1;
        if (free_pos && text && text[0] &&
            (a.x1 < 32 || a.x2 > kW - 1 - 32)) {
          char head[40] = "";
          std::snprintf(head, sizeof(head), "%.30s", text);
          for (char* q = head; *q; ++q)
            if (*q == '\n') *q = ' ';
          report_line("%s / margin / %s / (%d,%d)-(%d,%d) / \"%s\"\n",
                      s_audit_screen, kind, (int)a.x1, (int)a.y1,
                      (int)a.x2, (int)a.y2, head);
        }
      }
    }
  }
  // 子に渡す clip: スクロール可能な親は内容領域で子を切る、それ以外は
  // 子がはみ出せるので親の外形のまま。
  lv_area_t child_clip;
  if (lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE)) {
    lv_obj_get_content_coords(o, &child_clip);
  } else {
    child_clip = a;
  }
  child_clip.x1 = LV_MAX(child_clip.x1, clip.x1);
  child_clip.y1 = LV_MAX(child_clip.y1, clip.y1);
  child_clip.x2 = LV_MIN(child_clip.x2, clip.x2);
  child_clip.y2 = LV_MIN(child_clip.y2, clip.y2);
  const uint32_t n = lv_obj_get_child_count(o);
  for (uint32_t i = 0; i < n; ++i) {
    audit_walk(lv_obj_get_child(o, i), child_clip,
               in_scroll ||
                   lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE));
  }
}

// ---- T16: テキスト重なり / 欠けグリフ監査 -------------------------------
// 可視ラベル (clip 済みの見えている部分) を集めて、
//   1) ラベルどうしの bbox 交差 → "overlap"
//   2) フォント (フォールバック含む) に無いコードポイント → "glyph"
// を報告する。親子・同一オブジェクトは交差判定から除外。

struct LblInfo {
  lv_area_t ink;   // グリフの実描画 (ink) 範囲 — 行高パディングを除く
  lv_area_t vis;   // clip 済みのオブジェクト外接
  std::string text;
  lv_obj_t* obj;
};
std::vector<LblInfo> s_lbls;

// lv_area_intersect は private ヘッダ (lv_area_private.h) なので同名の
// 自前版を使う。
bool area_intersect(lv_area_t* r, const lv_area_t& a, const lv_area_t& b) {
  r->x1 = LV_MAX(a.x1, b.x1);
  r->y1 = LV_MAX(a.y1, b.y1);
  r->x2 = LV_MIN(a.x2, b.x2);
  r->y2 = LV_MIN(a.y2, b.y2);
  return r->x1 <= r->x2 && r->y1 <= r->y2;
}

uint32_t utf8_next(const char*& p) {
  const auto c = static_cast<unsigned char>(*p++);
  if (c < 0x80) return c;
  if ((c & 0xE0) == 0xC0) return ((c & 0x1F) << 6) | (*p++ & 0x3F);
  if ((c & 0xF0) == 0xE0) {
    const uint32_t v = ((c & 0x0F) << 12) |
                       ((*p++ & 0x3F) << 6) | (*p++ & 0x3F);
    return v;
  }
  if ((c & 0xF8) == 0xF0) {
    const uint32_t v = ((c & 0x07) << 18) | ((*p++ & 0x3F) << 12) |
                       ((*p++ & 0x3F) << 6) | (*p++ & 0x3F);
    return v;
  }
  return c;
}

void audit_label_check(lv_obj_t* o, const lv_area_t& clip) {
  if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  if (lv_obj_check_type(o, &lv_label_class)) {
    const char* t = lv_label_get_text(o);
    lv_area_t vis;
    const bool has_vis =
        area_intersect(&vis, a, clip) && (t && t[0]);
    if (has_vis) {
      // ink 範囲 (各グリフの letter_pos + glyph box の和)。
      // ラベル外接より狭いので、行高パディングの重なりを拾わない。
      const lv_font_t* f =
          lv_obj_get_style_text_font(o, LV_PART_MAIN);
      lv_area_t content;
      lv_obj_get_content_coords(o, &content);
      lv_area_t ink{0, 0, -1, -1};
      bool any = false;
      {
        const char* q = t;
        uint32_t ci = 0;
        while (*q && ci < 64) {
          const uint32_t cp = utf8_next(q);
          lv_point_t lp;
          lv_label_get_letter_pos(o, ci, &lp);
          lv_font_glyph_dsc_t d;
          if (f && lv_font_get_glyph_dsc(f, &d, cp, 0)) {
            lv_area_t g{
                static_cast<lv_coord_t>(content.x1 + lp.x + d.ofs_x),
                static_cast<lv_coord_t>(content.y1 + lp.y + d.ofs_y),
                static_cast<lv_coord_t>(content.x1 + lp.x + d.ofs_x +
                                        d.box_w - 1),
                static_cast<lv_coord_t>(content.y1 + lp.y + d.ofs_y +
                                        d.box_h - 1)};
            if (!any) { ink = g; any = true; }
            else {
              ink.x1 = LV_MIN(ink.x1, g.x1);
              ink.y1 = LV_MIN(ink.y1, g.y1);
              ink.x2 = LV_MAX(ink.x2, g.x2);
              ink.y2 = LV_MAX(ink.y2, g.y2);
            }
          }
          ++ci;
        }
      }
      lv_area_t ink_vis;
      if (any && area_intersect(&ink_vis, ink, clip)) {
        s_lbls.push_back({ink_vis, vis, t, o});
      }
      // 欠けグリフ検査 (フォールバックも lv_font_get_glyph_dsc が辿る。
      // 実グリフ無しで placeholder が返ると is_placeholder=1。
      // 制御文字 (\n 等) はグリフ不要なので除外)。
      if (f) {
        const char* q = t;
        while (*q) {
          const uint32_t cp = utf8_next(q);
          if (cp < 0x20) continue;
          lv_font_glyph_dsc_t d;
          const bool okd = lv_font_get_glyph_dsc(
              f, &d, cp, 0);
          if (!okd || d.is_placeholder) {
            char head[40] = "";
            std::snprintf(head, sizeof(head), "%.30s", t);
            report_line(
                "%s / glyph / U+%04X / \"%s\"\n",
                s_audit_screen, (unsigned)cp, head);
            break;  // 1 ラベル 1 報告で十分
          }
        }
      }
    }
  }
  // 子に渡す clip は audit_walk と同じ規則。
  lv_area_t child_clip;
  if (lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE)) {
    lv_obj_get_content_coords(o, &child_clip);
  } else {
    child_clip = a;
  }
  child_clip.x1 = LV_MAX(child_clip.x1, clip.x1);
  child_clip.y1 = LV_MAX(child_clip.y1, clip.y1);
  child_clip.x2 = LV_MIN(child_clip.x2, clip.x2);
  child_clip.y2 = LV_MIN(child_clip.y2, clip.y2);
  const uint32_t n = lv_obj_get_child_count(o);
  for (uint32_t i = 0; i < n; ++i) {
    audit_label_check(lv_obj_get_child(o, i), child_clip);
  }
}

void audit_overlap() {
  for (size_t i = 0; i < s_lbls.size(); ++i) {
    for (size_t j = i + 1; j < s_lbls.size(); ++j) {
      // 片方がもう一方の祖先/子孫なら除外 (行内ラベル等の重なりは
      // レイアウト由来でなく実際に食い違っているものだけ拾う)。
      lv_obj_t* a = s_lbls[i].obj;
      lv_obj_t* b = s_lbls[j].obj;
      bool related = false;
      for (lv_obj_t* p = lv_obj_get_parent(a); p;
           p = lv_obj_get_parent(p)) {
        if (p == b) { related = true; break; }
      }
      for (lv_obj_t* p = lv_obj_get_parent(b); p && !related;
           p = lv_obj_get_parent(p)) {
        if (p == a) { related = true; break; }
      }
      if (related) continue;
      lv_area_t x;
      if (area_intersect(&x, s_lbls[i].ink, s_lbls[j].ink)) {
        report_line(
            "%s / overlap / (%d,%d)-(%d,%d) x (%d,%d)-(%d,%d) / "
            "\"%.24s\" vs \"%.24s\"\n",
            s_audit_screen,
            (int)s_lbls[i].ink.x1, (int)s_lbls[i].ink.y1,
            (int)s_lbls[i].ink.x2, (int)s_lbls[i].ink.y2,
            (int)s_lbls[j].ink.x1, (int)s_lbls[j].ink.y1,
            (int)s_lbls[j].ink.x2, (int)s_lbls[j].ink.y2,
            s_lbls[i].text.c_str(), s_lbls[j].text.c_str());
      }
    }
  }
}

void audit_screen(const char* name) {
  if (!s_report) return;
  s_audit_screen = name;
  const lv_area_t scr_clip{0, 0, kW - 1, kH - 1};
  audit_walk(lv_screen_active(), scr_clip, false);
  audit_walk(lv_layer_top(), scr_clip,
             false);  // トースト/アラートは layer_top に載る
  // 層ごとに別々に (layer_top のモーダルと背後画面の重なりは当然なので
  // 別レイヤ同士は比較しない)。
  s_lbls.clear();
  audit_label_check(lv_screen_active(), scr_clip);
  audit_overlap();
  s_lbls.clear();
  audit_label_check(lv_layer_top(), scr_clip);
  audit_overlap();
}

bool save(const char* dir, const char* name) {
  char path[512];
  std::snprintf(path, sizeof(path), "%s/%s.png", dir, name);
  const bool ok = write_png_rgb565(path, s_fb, kW, kH);
  audit_screen(name);
  std::printf("%s -> %s\n", name, ok ? "ok" : "FAILED");
  return ok;
}

void nav_to(watch::Route r) {
  ui::emit(watch::ActionType::Navigate, static_cast<uint32_t>(r));
  pump(400);  // アニメ 220ms + 余裕
}

void back_home() {
  ui::emit(watch::ActionType::Home);
  pump(400);
}

void tap(int x, int y) {
  s_ptr = {static_cast<int16_t>(x), static_cast<int16_t>(y), true};
  pump(60);
  s_ptr.pressed = false;
  pump(80);
}

// 歩行っぽい ImuSample を n サンプル流す (50ms 間隔想定)。
// 大きさが 1400/600mg を 5 サンプルごとに振動 → 約 1 歩/10 サンプル。
#if SAS_APP_STEPS
void feed_walk(int n) {
  for (int i = 0; i < n; ++i) {
    watch::Action a{};
    a.type = watch::ActionType::ImuSample;
    a.source = watch::ActionSource::System;  // 計測は電源を蹴らない
    const int16_t z =
        static_cast<int16_t>(-1000 + ((i % 10 < 5) ? -400 : 400));
    a.arg0 = 0;
    a.arg1 = static_cast<uint16_t>(z);
    s_rt.queue().push(a);
    pump(50);
  }
}
#endif  // SAS_APP_STEPS

// ---- parts ギャラリー ----------------------------------------------
// 全スキンパーツを 1 画面に並べるデバッグ画面。LV_STATE_* を強制付与して
// state 画像差替え (pressed/checked/disabled) を目視確認する。

lv_obj_t* gbox(lv_obj_t* p, int32_t w, int32_t h) {
  lv_obj_t* o = lv_obj_create(p);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

lv_obj_t* grow3(lv_obj_t* p, int32_t h) {
  lv_obj_t* r = gbox(p, 346, h);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(r, 6, 0);
  return r;
}

void glabel(lv_obj_t* p, const char* s) {
  lv_obj_t* l = lv_label_create(p);
  lv_label_set_text(l, s);
  lv_obj_set_style_text_font(l, ui::theme().font_body, 0);
  lv_obj_center(l);
}

lv_obj_t* gpart(lv_obj_t* p, watch::ThemeSkinPartId part, int32_t w,
                int32_t h, lv_state_t st, const char* text) {
  lv_obj_t* o = gbox(p, w, h);
  ui::c::skin_obj(o, part);
  if (st) lv_obj_add_state(o, st);
  if (text) glabel(o, text);
  return o;
}

void parts_gallery(const char* out_dir, const char* theme_id, int num,
                   bool* ok) {
  lv_obj_t* prev = lv_screen_active();
  lv_obj_t* g = gbox(nullptr, kW, kH);
  lv_obj_set_style_bg_color(g, ui::theme().bg, 0);
  lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
  ui::c::header(g, "パーツ", true);  // header_bar + back_pill

  lv_obj_t* col = gbox(g, 346, 464 - 78);
  lv_obj_set_pos(col, 32, 78);
  // 下端は丸角の安全域 (y<=464) で止め、あふれた分はスクロールにする。
  lv_obj_add_flag(col, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(col, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(col, 3, 0);

  ui::c::caption(col, "ボタン / スイッチ / スライダー");

  // ボタン 3 種 x normal/pressed/disabled。
  using MkBtn = lv_obj_t* (*)(lv_obj_t*, const char*, lv_event_cb_t, void*);
  const struct {
    MkBtn mk;
    const char* tx;
  } btns[3] = {{ui::c::button_primary, "主"},
               {ui::c::button, "副"},
               {ui::c::button_danger, "危"}};
  const lv_state_t sts[3] = {LV_STATE_DEFAULT, LV_STATE_PRESSED,
                             LV_STATE_DISABLED};
  for (const auto& b : btns) {
    lv_obj_t* r = grow3(col, 30);
    for (int i = 0; i < 3; ++i) {
      lv_obj_t* o = b.mk(r, b.tx, nullptr, nullptr);
      // 列幅は content 幅 (346) から計算: (346 - 2*gap 6) / 3。
      lv_obj_set_size(o, (346 - 12) / 3, 30);
      if (sts[i]) lv_obj_add_state(o, sts[i]);
    }
  }

  // スイッチ off/on/disabled + icon_tile + toast。
  lv_obj_t* ra = grow3(col, 36);
  lv_obj_t* sw1 = ui::c::mk_switch(ra, false);
  lv_obj_t* sw2 = ui::c::mk_switch(ra, true);
  lv_obj_t* sw3 = ui::c::mk_switch(ra, true);
  lv_obj_add_state(sw3, LV_STATE_DISABLED);
  (void)sw1;
  (void)sw2;
  gpart(ra, watch::kSkinIconTile, 36, 36, LV_STATE_DEFAULT, "タ");
  gpart(ra, watch::kSkinToast, 142, 36, LV_STATE_DEFAULT, "通知");

  // back_pill x 3 state。
  lv_obj_t* rb = grow3(col, 30);
  gpart(rb, watch::kSkinBackPill, 96, 30, LV_STATE_DEFAULT, "戻る");
  gpart(rb, watch::kSkinBackPill, 96, 30, LV_STATE_PRESSED, "戻る");
  gpart(rb, watch::kSkinBackPill, 96, 30, LV_STATE_DISABLED, "戻る");

  // スライダー (track/fill/knob)。
  ui::c::slider_row(col, "明るさ", "%", 0, 100, 60, nullptr, nullptr);

  // 小さいサイズ: ボタン h40 (長いラベルは末尾 … になる) + カード h40
  // + 行 h40 (radius が高さ/2 に丸められるかの確認用)。
  ui::c::caption(col, "小さいサイズ / 状態");
  lv_obj_t* rs = grow3(col, 40);
  {
    // 列幅は content 幅 346 から導出 (gap 6 × 2)。
    lv_obj_t* o = ui::c::button_primary(rs, "とても長いボタンラベルの例",
                                        nullptr, nullptr);
    lv_obj_set_size(o, 184, 40);
    lv_obj_t* o2 = ui::c::button(rs, "副", nullptr, nullptr);
    lv_obj_set_size(o2, (346 - 184 - 12) / 2, 40);
    lv_obj_t* o3 = ui::c::button_danger(rs, "危", nullptr, nullptr);
    lv_obj_set_size(o3, (346 - 184 - 12) / 2, 40);
    lv_obj_add_state(o3, LV_STATE_PRESSED);
  }
  lv_obj_t* rs2 = grow3(col, 40);
  {
    lv_obj_t* cd = ui::c::card(rs2);
    lv_obj_set_size(cd, 184, 40);
    glabel(cd, "小カード");
    lv_obj_t* sw = ui::c::mk_switch(rs2, true);
    lv_obj_set_size(sw, 46, 26);  // 小スイッチ (knob は track−2·pad で導出)
  }

  // グループ: list_group + row + divider + icon_tile。
  lv_obj_t* grp = ui::c::group(col);
  lv_obj_t* ri = ui::c::row_icon(grp, "時", lv_color_hex(0x445566), "アプリ",
                               "sub", true, nullptr, nullptr);
  lv_obj_set_style_min_height(ri, 44, 0);
  lv_obj_t* rp = ui::c::row(grp, "押下状態", nullptr, "v", true, nullptr,
                            nullptr);
  lv_obj_set_style_min_height(rp, 34, 0);
  lv_obj_add_state(rp, LV_STATE_PRESSED);
  lv_obj_t* rd = ui::c::row(grp, "無効状態", nullptr, nullptr, false, nullptr,
                            nullptr);
  lv_obj_set_style_min_height(rd, 34, 0);
  lv_obj_add_state(rd, LV_STATE_DISABLED);

  // card + bubble。
  lv_obj_t* rc = grow3(col, 44);
  lv_obj_t* cd = ui::c::card(rc);
  lv_obj_set_size(cd, 172, 44);
  glabel(cd, "カード");
  gpart(rc, watch::kSkinBubble, 188, 44, LV_STATE_DEFAULT, "ふきだし");

  lv_screen_load(g);
  pump(400);
  char nm[64];
  std::snprintf(nm, sizeof(nm), "%d_theme_%s_parts", num, theme_id);
  *ok &= save(out_dir, nm);
  lv_screen_load(prev);
  lv_obj_delete(g);
  pump(200);
}

// 9-slice 継ぎ目の回帰チェック。scale 丸めで領域境に未描画の1px抜け
// (両隣より暗い列/行) が出ないことを、バグが実際に出た形状
// (card 内側 334x58 の button_primary) でピクセル検査する。
bool seam_check() {
  lv_obj_t* prev = lv_screen_active();
  lv_obj_t* scr = lv_obj_create(nullptr);
  lv_obj_set_size(scr, kW, kH);
  lv_obj_set_style_bg_color(scr, ui::theme().bg, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_screen_load(scr);
  lv_obj_t* b = lv_obj_create(scr);
  lv_obj_set_size(b, 334, 58);
  lv_obj_set_pos(b, 38, 200);
  ui::c::skin_obj(b, watch::kSkinBtnPrimary);
  pump(200);
  const watch::ThemeSkinPart& sp =
      ui::theme().skin_part[watch::kSkinBtnPrimary];
  const bool skinned = (sp.set & 0x03) == 0x03 &&
                       ui::theme().skin[watch::kSkinBtnPrimary];
  int bad = 0;
  if (skinned) {
    const int l = sp.slice[0], tp = sp.slice[1], r = sp.slice[2],
              bt = sp.slice[3];
    const int x1 = 38 + l + 2, x2 = 38 + 334 - r - 3;
    const int y1 = 200 + tp + 2, y2 = 200 + 58 - bt - 3;
    auto lum = [&](int x, int y) {
      const uint16_t p = s_fb[y * kW + x];
      return ((p >> 11) & 0x1F) * 8 + ((p >> 5) & 0x3F) * 4 + (p & 0x1F) * 8;
    };
    for (int x = x1; x <= x2; ++x) {
      int hits = 0;
      for (int y = y1; y <= y2; ++y) {
        if (lum(x, y) < lum(x - 1, y) - 24 &&
            lum(x, y) < lum(x + 1, y) - 24)
          ++hits;
      }
      if (hits > 6) {
        std::printf("seam_check: dark col x=%d hits=%d\n", x, hits);
        ++bad;
      }
    }
    for (int y = y1; y <= y2; ++y) {
      int hits = 0;
      for (int x = x1; x <= x2; ++x) {
        if (lum(x, y) < lum(x, y - 1) - 24 &&
            lum(x, y) < lum(x, y + 1) - 24)
          ++hits;
      }
      if (hits > 6) {
        std::printf("seam_check: dark row y=%d hits=%d\n", y, hits);
        ++bad;
      }
    }
  }
  lv_screen_load(prev);  // 先に戻す (ロード中画面の delete は不可)。
  lv_obj_delete(scr);
  pump(100);
  return bad == 0;
}

}  // namespace

int main(int argc, char** argv) {
  const char* out = "docs/screenshots";
  const char* only_theme = nullptr;  // --theme <id> でテーマ節を絞る
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
      out = argv[++i];
    } else if (std::strcmp(argv[i], "--theme") == 0 && i + 1 < argc) {
      only_theme = argv[++i];
    }
  }
  std::filesystem::create_directories(out);
  const auto want_theme = [only_theme](const char* id) {
    return !only_theme || std::strcmp(only_theme, id) == 0;
  };
  {
    char rp[512];
    std::snprintf(rp, sizeof(rp), "%s/safe_area_report.txt", out);
    s_report = std::fopen(rp, "w");
  }

  watch::log_set(&sim::log());

  // 表示 410x502 RGB565。
  lv_init();
  lv_tick_set_cb([]() -> uint32_t {
    return static_cast<uint32_t>(sim::clock().now_ms());
  });
  s_disp = lv_display_create(kW, kH);
  lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(s_disp, flush_cb);
  lv_display_set_buffers(s_disp, s_draw, nullptr, sizeof(s_draw),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  s_indev = lv_indev_create();
  lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(s_indev, read_cb);
  lv_indev_set_display(s_indev, s_disp);

  // core 起動 (2025-10-05 09:41 UTC, JST 表示で 18:41)。
  sim::clock().set_base_epoch(1759657260);
  s_settings.tz_offset_min = 540;  // JST
  static watch::FeatureContext fctx{s_bus, sim::kv(), sim::clock(),
                                    &s_nav, &s_power};
  fctx.audio = &sim::audio();
  fctx.settings = &s_settings;
  s_fctx = &fctx;
  s_rt.init(fctx);
  s_features.restore_all(fctx);

#if SAS_APP_MEMO
  // メモを2件仕込んでおく (一覧が見えるように)。
  const char* m1 = "買い物: 牛乳、卵、食パン";
  const char* m2 = "TODO: 実機が届いたら輝度・ジェスチャーの確認をする";
  watch::features::memo_create(m1, std::strlen(m1), fctx);
  watch::features::memo_create(m2, std::strlen(m2), fctx);
#endif

  // アラーム 2件 (7:00 平日 / 9:30 毎日) と通知・曲情報。
#if SAS_APP_ALARM
  watch::features::alarm_set(0, 7, 0, 0x3E, true, fctx);
  watch::features::alarm_set(0, 9, 30, 0, false, fctx);
#endif
#if SAS_APP_NOTIFY
  watch::features::notify_add("LINE", "母", "夕飯何にする？");
  watch::features::notify_add("Gmail", "GitHub", "[SAS-Watch] PR #9 merged");
#endif
#if SAS_APP_MEDIA
  watch::features::media_set("夜に駆ける", "YOASOBI", true, 0, fctx);
#endif

  // 文字盤の補助データ (ui/face_data.hpp)。歩数・通知数・次のアラーム
  // は実 Feature に結線 (alarm_next_fire_epoch → 当日 min-of-day)。
  static const ui::face_data::Hooks kFaceData = {
      []() -> int32_t {
#if SAS_APP_STEPS
        return static_cast<int32_t>(watch::features::steps_today());
#else
        return -1;
#endif
      },
      []() -> int32_t { return static_cast<int32_t>(s_settings.steps_goal); },
      []() -> int32_t {
#if SAS_APP_NOTIFY
        return static_cast<int32_t>(watch::features::notify_count());
#else
        return -1;
#endif
      },
      []() -> int32_t {
#if SAS_APP_ALARM
        const int64_t e = watch::features::alarm_next_fire_epoch();
        if (e <= 0) return -1;
        const int64_t local = e + s_settings.tz_offset_min * 60;
        return static_cast<int32_t>(((local % 86400) + 86400) % 86400 / 60);
#else
        return -1;
#endif
      },
  };
  ui::face_data::set_hooks(&kFaceData);

  // Action 出口 → core キュー。
  ui::set_action_sink(&sink);
  ui::create({&s_bus, &s_nav, &s_settings, &fctx});
  pump(400);

  bool ok = true;
  ok &= save(out, "01_home");

  nav_to(watch::Route::Quick);
  ok &= save(out, "02_quick");

  back_home();
  nav_to(watch::Route::More);
  ok &= save(out, "03_applist");

#if SAS_APP_TIMER
  nav_to(watch::Route::Timer);
  ok &= save(out, "04_timer_setup");

  // 3分タイマーを開始して 90 秒経過させる (残り 1:30 → リングが半周、
  // 未充填トラックも写る状態で撮る)。無入力だと 12 秒で画面OFFになり、
  // 消灯中の Action は「起こすだけ」で捨てられる (TimerReset が効かず
  // タイマーが動き続ける) ため、間だけ PowerPolicy の消灯を延ばす。
  s_power.configure(8, 300, 1800);
  ui::emit(watch::ActionType::TimerStart, 180);
  pump(90500);
  ok &= save(out, "05_timer_running");
  s_power.configure(8, 12, 1800);

  ui::emit(watch::ActionType::TimerReset);
#endif
#if SAS_APP_STOPWATCH
  back_home();
  nav_to(watch::Route::Stopwatch);
  ok &= save(out, "06_stopwatch");

  ui::emit(watch::ActionType::StopwatchToggle);
  pump(400);
  ui::emit(watch::ActionType::StopwatchLap);
  pump(400);
  ok &= save(out, "07_stopwatch_running");

  ui::emit(watch::ActionType::StopwatchReset);
#endif
#if SAS_APP_COUNTER
  back_home();
  nav_to(watch::Route::Counter);
  ui::emit(watch::ActionType::CounterAdd, 1);
  ui::emit(watch::ActionType::CounterAdd, 1);
  ui::emit(watch::ActionType::CounterAdd, 1);
  pump(200);
  ok &= save(out, "08_counter");
  // リセット → 確認ダイアログ (T14)。「いいえ」で閉じる。
  tap(205, 350);
  pump(200);
  ok &= save(out, "32_counter_confirm");
  tap(205, 305);
  pump(200);
#endif

#if SAS_APP_MEMO
  back_home();
  nav_to(watch::Route::Memo);
  ok &= save(out, "09_memo_list");
  // 先頭行をタップして詳細 (録音カード ~140px の下から行が始まる)。
  tap(205, 210);
  pump(200);
  ok &= save(out, "10_memo_detail");
  // 「削除」→ 確認ダイアログ (T14)。「いいえ」で閉じる。
  tap(205, 200);
  pump(200);
  ok &= save(out, "33_memo_confirm");
  tap(205, 305);
  pump(200);
  // 「一覧に戻る」ボタン (詳細カード末尾) をタップ。
  tap(205, 249);
  pump(200);
#endif

  back_home();
  nav_to(watch::Route::Settings);
  ok &= save(out, "11_settings");

  // 「文字盤」「数字フォント」は行タップで選択ビューに開く (掘り下げ)。
  // 実測: 文字盤行センター y≈338、数字フォント y≈399、選択ビューの戻る行 y≈106。
  tap(205, 338);  // 文字盤
  pump(200);
  ok &= save(out, "30_settings_faces");
  tap(205, 106);  // ‹ 戻る
  pump(200);
  tap(205, 399);  // 数字フォント
  pump(200);
  ok &= save(out, "31_settings_fonts");
  tap(205, 106);  // ‹ 戻る
  pump(200);

  nav_to(watch::Route::PowerMenu);
  ok &= save(out, "12_powermenu");

  // パスキー確認モーダル。
  back_home();
  ui::request_passkey(483920);
  pump(300);
  ok &= save(out, "13_passkey");
  // 「はい」ボタン (下端 -86 のピル → 中心 y≈387) をタップして閉じる。
  // 閉じ損なうと layer_top の全画面モーダルが残って以降の shot を覆う。
  tap(205, 387);
  pump(200);

  // タイマー終了フルスクリーン通知。
  back_home();
  s_bus.publish({watch::EventType::TimerFinished, 0});
  pump(300);
  ok &= save(out, "14_timer_alert");
  // T9: 点滅の 2 フレーム目 (500ms 周期 → 400ms 後は反対色)。
  pump(400);
  ok &= save(out, "14b_timer_alert_flash");
  tap(205, 387);  // 止める
  pump(200);

  // 内蔵 standard のパーツギャラリー (テーマ別ギャラリーは各節で撮る)。
  parts_gallery(out, "standard", 90, &ok);

  // ---- テーマ切替 (docs/theme-format.md) ----
  if (want_theme("light")) {
    // 内蔵の明るめテーマ。
    back_home();
    ui::emit_text(watch::ActionType::SetTheme, "light");
    pump(500);
    ok &= save(out, "15_theme_light_home");
    nav_to(watch::Route::Settings);
    ok &= save(out, "16_theme_light_settings");

    parts_gallery(out, "light", 88, &ok);
  }

  if (want_theme("mame")) {
    // サンプルキャラテーマ (sim/themes/mame。tools/build_themes.py で生成)。
    back_home();
    ui::emit_text(watch::ActionType::SetTheme, "mame");
    pump(500);
    ok &= save(out, "17_theme_mame_home");
    s_bus.publish({watch::EventType::TimerFinished, 0});
    pump(300);
    ok &= save(out, "18_theme_mame_timer");
    tap(205, 387);  // 止める
    pump(200);
    // T20: assets 経由の遅延ロードを確認 (mame は home_bg を持つ)。
    std::printf("mame theme_image(home_bg): %s\n",
                ui::theme_image("home_bg") ? "ok" : "FAILED");
    if (!ui::theme_image("home_bg")) ok = false;
  }

  // 元に戻しておく。
  ui::emit_text(watch::ActionType::SetTheme, "standard");
  pump(400);

#if SAS_APP_MEMO
  // 音声メモ: 録音中の表示 → 確定 → 音声メモ詳細 (再生ボタン)。
  back_home();
  nav_to(watch::Route::Memo);
  ui::emit(watch::ActionType::MemoRecordStart);
  pump(2500);
  ok &= save(out, "19_memo_recording");
  ui::emit(watch::ActionType::MemoRecordStop);
  pump(400);
  // 先頭行 (新しい音声メモ) を開く。
  tap(205, 210);
  pump(300);
  ok &= save(out, "20_memo_voice_detail");
#endif

  // ファーム更新画面: 待機 → 進捗中。
  back_home();
  nav_to(watch::Route::Ota);
  ok &= save(out, "21_ota_idle");
  sim::set_ota_debug(2, 42, "", "0.2.0");
  s_bus.publish({watch::EventType::OtaProgress, 42});
  pump(300);
  ok &= save(out, "22_ota_progress");

#if SAS_APP_ALARM
  // ---- アラーム ----
  back_home();
  nav_to(watch::Route::Alarm);
  ok &= save(out, "40_alarm");

  // 鳴動アラート: 今この分のアラームを登録すると次の tick で鳴る。
  watch::features::alarm_set(0, 18, 41, 0, true, fctx);
  pump(400);
  ok &= save(out, "41_alarm_alert");
  tap(205, 387);  // 止める (下端 -86 のピル中心)
  pump(300);
#endif

#if SAS_APP_NOTIFY
  // ---- 通知一覧 ----
  nav_to(watch::Route::Notifications);
  ok &= save(out, "42_notifications");
  // 先頭行をタップして詳細 (アプリ名・タイトル・本文の全体表示)。
  tap(205, 130);
  pump(200);
  ok &= save(out, "43_notification_detail");
  tap(205, 212);  // 一覧に戻る (detail カード内のボタン y≈188-236。
                  // 290 は列末尾の「すべて消す」を踏む)
  pump(200);

  // 通知ポップアップ: notify.post 相当 (store 追加 + NotificationPosted)。
  watch::features::notify_add("LINE", "兄", "今週末帰るよ");
  s_bus.publish({watch::EventType::NotificationPosted, 0});
  pump(300);
  ok &= save(out, "44_notify_popup");
  pump(4500);  // トーストが消えるまで待つ
#endif

#if SAS_APP_MEDIA
  // ---- 音楽操作 ----
  back_home();
  nav_to(watch::Route::Media);
  ok &= save(out, "45_media");
#endif

#if SAS_APP_STEPS
  // 歩数: 歩行っぽい加速度を流してから歩数画面を開く。
  // (ImuSample ���電源を蹴らないので、途中で PowerState は ScreenOff へ
  //  進んでいる。先に Wake で起こしてから遷移する。)
  back_home();
  feed_walk(24000);  // ~2400 歩 (目標 8000 の ~30%)
  ui::emit(watch::ActionType::Wake);
  pump(200);
  nav_to(watch::Route::Steps);
  ok &= save(out, "23_steps");
#endif

  // ---- 文字盤 (settings.face) ----
  // standard テーマで4面 (01_home = bold と同じ見えになるが、
  // 文字盤名を揃えて残すため明示的に撮る)。
  back_home();
  ui::emit_text(watch::ActionType::SetTheme, "standard");
  pump(400);
  const char* kFaceShots[][2] = {
      {"bold", "24_face_bold"},
      {"analog", "25_face_analog"},
      {"hud", "26_face_hud"},
      {"minimal", "27_face_minimal"},
  };
  for (const auto& fs : kFaceShots) {
    ui::emit_text(watch::ActionType::SetFace, fs[0]);
    pump(300);
    ok &= save(out, fs[1]);
  }
  // chara_* は立ち絵 (face_chara) のある mame テーマで。
  ui::emit_text(watch::ActionType::SetTheme, "mame");
  pump(500);
  ui::emit_text(watch::ActionType::SetFace, "chara_side");
  pump(300);
  ok &= save(out, "28_face_chara_side");
  ui::emit_text(watch::ActionType::SetFace, "chara_bubble");
  pump(300);
  ok &= save(out, "29_face_chara_bubble");
  // 元に戻す。
  ui::emit_text(watch::ActionType::SetFace, "bold");
  ui::emit_text(watch::ActionType::SetTheme, "standard");
  pump(300);


#if SAS_APP_AGENT
  // ---- AI (Agent) ----
  // sim には BLE が無いので、ble_connected を立てておき、送信完了/返答は
  // feature の API を直接叩く (実機では ble_glue が agent_pending を拾う)。
  s_power.set_ble_connected(true);
  back_home();
  nav_to(watch::Route::Agent);
  ok &= save(out, "50_agent");

  ui::emit(watch::ActionType::AgentRecordToggle);
  pump(2500);
  ok &= save(out, "51_agent_recording");
  ui::emit(watch::ActionType::AgentRecordToggle);  // 停止→送信中
  pump(300);

  // 定型質問で「考え中→返答」まで再現する。
  watch::features::agent_reset_state();
  ui::emit(watch::ActionType::AgentAsk, 0);  // agent.q1
  pump(300);
  watch::features::AgentPending ap;
  if (watch::features::agent_pending(&ap)) {
    watch::features::agent_send_started(ap.id);
    watch::features::agent_sent(ap.id, true, *s_fctx);
  }
  pump(100);
  ok &= save(out, "52_agent_thinking");
  const char* reply =
      "今日は 15 時にミーティング、19 時にジムの予定があります。";
  watch::features::agent_on_reply(ap.id, reply, std::strlen(reply),
                                  *s_fctx);
  pump(300);
  ok &= save(out, "53_agent_reply");
#endif

  // ---- テーマ v2 サンプル「cosmos」 ----
  // sim/themes/cosmos.zip を zip 直読み (展開済み dir は読まない)。
  // screens/style/icons/fonts/mascot/face_layout の v2 全キーを行使。
  if (want_theme("cosmos")) {
  back_home();
  ui::emit_text(watch::ActionType::SetFace, "theme");  // face_layout 駆動
  ui::emit_text(watch::ActionType::SetTheme, "cosmos");
  pump(800);
  ok &= save(out, "60_theme_cosmos_home");   // home + theme 文字盤

  nav_to(watch::Route::More);
  pump(200);
  ok &= save(out, "61_theme_cosmos_applist");  // icons + mascot

#if SAS_APP_TIMER
  nav_to(watch::Route::Timer);
  ok &= save(out, "62_theme_cosmos_timer");    // 画面別 bg + mascot
#endif

#if SAS_APP_MEMO
  back_home();
  nav_to(watch::Route::Memo);
  pump(200);
  ok &= save(out, "63_theme_cosmos_memo");
#endif

  back_home();
  nav_to(watch::Route::Settings);
  ok &= save(out, "64_theme_cosmos_settings");  // scrim 高め + style

  // タイマー終了アラート (screens.alert bg + images.timer_done + mascot)。
  back_home();
  s_bus.publish({watch::EventType::TimerFinished, 0});
  pump(300);
  ok &= save(out, "65_theme_cosmos_alert");
  tap(205, 387);  // 止める
  pump(300);

  // cosmos の face_chara は images スロット経由で既存文字盤にも効く。
  // chara 系レイアウトを cosmos 背景で撮る。
  back_home();
  ui::emit_text(watch::ActionType::SetFace, "chara_side");
  pump(300);
  ok &= save(out, "66_theme_cosmos_face_chara_side");
  ui::emit_text(watch::ActionType::SetFace, "chara_bubble");
  pump(300);
  ok &= save(out, "67_theme_cosmos_face_chara_bubble");

  // quick 設定画面も撮る (mascot 対象外画面の見え確認)。
  ui::emit_text(watch::ActionType::SetFace, "theme");
  nav_to(watch::Route::Quick);
  pump(200);
  ok &= save(out, "68_theme_cosmos_quick");
  }  // want_theme("cosmos")

  // ---- テーマ v3 サンプル「cyber」「cute」 ----
  // sim/themes/<id>.zip (tools/build_themes.py --kit が kit.html から生成)。
  // skin の 9-slice 画像スキンが全コンポーネントに効く。
  for (const char* theme_id : {"cyber", "cute"}) {
    if (!want_theme(theme_id)) continue;
    const bool cy = theme_id[0] == 'c' && theme_id[1] == 'y';
    int n = cy ? 70 : 80;
    char nm[64];
    auto shot = [&](const char* tail) {
      std::snprintf(nm, sizeof(nm), "%d_theme_%s_%s", n++, theme_id, tail);
      ok &= save(out, nm);
    };

    back_home();
    ui::emit_text(watch::ActionType::SetFace, "bold");  // face_layout は無い
    ui::emit_text(watch::ActionType::SetTheme, theme_id);
    pump(800);
    shot("home");  // 画面 bg + スキン

    nav_to(watch::Route::More);
    pump(200);
    shot("applist");  // list_group / row / icon_tile

    back_home();
    nav_to(watch::Route::Settings);
    pump(200);
    shot("settings");  // switch on/off + slider + caption_line

#if SAS_APP_TIMER
    back_home();
    nav_to(watch::Route::Timer);
    pump(200);
    shot("timer");  // button_primary/secondary
#endif

    back_home();
    nav_to(watch::Route::Quick);
    pump(200);
    shot("quick");

#if SAS_APP_MEMO
    back_home();
    nav_to(watch::Route::Memo);
    pump(200);
    shot("memo");
#endif

    // タイマー終了アラート (button_primary/danger + bubble skin)。
    back_home();
    s_bus.publish({watch::EventType::TimerFinished, 0});
    pump(300);
    shot("alert");
    tap(205, 387);  // 止める
    pump(300);

    // 全パーツ x 全 state のギャラリー (77 / 87)。
    parts_gallery(out, theme_id, n++, &ok);

    // 9-slice 継ぎ目の回帰チェック (バグが出た 334x58 ボタンを検査)。
    ok &= seam_check();

    std::printf("%s skin bytes: %u\n", theme_id,
                static_cast<unsigned>(ui::theme_skin_bytes()));
    // T20: assets/theme_image の疎通 ("home_bg" は images スロットの別名)。
    std::printf("%s theme_image(home_bg): %s\n", theme_id,
                ui::theme_image("home_bg") ? "ok" : "-");
  }

  // ---- 任意テーマ (tools/theme-studio の書き出し等) ----
  // --theme <id> で上記の専用セクションに無い id を指定すると
  // ここで汎用巡回 (home/settings/parts) を撮る。
  if (only_theme && std::strcmp(only_theme, "standard") != 0 &&
      std::strcmp(only_theme, "light") != 0 &&
      std::strcmp(only_theme, "mame") != 0 &&
      std::strcmp(only_theme, "cosmos") != 0 &&
      std::strcmp(only_theme, "cyber") != 0 &&
      std::strcmp(only_theme, "cute") != 0) {
    back_home();
    ui::emit_text(watch::ActionType::SetFace, "bold");
    ui::emit_text(watch::ActionType::SetTheme, only_theme);
    pump(800);
    ok &= save(out, "90_theme_custom_home");
    nav_to(watch::Route::Settings);
    ok &= save(out, "91_theme_custom_settings");
    back_home();
    parts_gallery(out, only_theme, 95, &ok);
  }

  // 戻して終了。
  ui::emit_text(watch::ActionType::SetFace, "bold");
  ui::emit_text(watch::ActionType::SetTheme, "standard");
  pump(400);

  if (s_report) {
    std::fclose(s_report);
    s_report = nullptr;
  }
  // T16: 監査違反も失敗にする (重なり/欠けグリフ/safe_area/margin 全て)。
  const bool clean = ok && s_report_lines == 0;
  std::printf("done -> %s (%s, audit %d lines)\n", out,
              clean ? "ok" : "some failed", s_report_lines);
  return clean ? 0 : 1;
}
