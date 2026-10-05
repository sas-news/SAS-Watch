// face_chara_bubble.cpp — 「chara_bubble」文字盤。
//   上に時刻と日付、右下に立ち絵 (face_chara / 無ければ stand)、
//   左に白い角丸ふきだし+しっぽ。ふきだしの文言は時間帯
//   (朝/昼/夕/夜) と歩数目標の残りから theme.bubble[] で選ぶ。
//   下に電池。画像が無いテーマでは時刻中央の簡易レイアウトに倒す。
#include "faces.hpp"

#include <cstdio>
#include <cstring>

#include "ui/face_data.hpp"
#include "ui/port.hpp"
#include "watch/power.hpp"

namespace {
using ui::face::Now;

struct S {
  bool has_img = false;
  lv_obj_t* time = nullptr;
  lv_obj_t* date = nullptr;
  lv_obj_t* bubble_l = nullptr;
  lv_obj_t* batt = nullptr;
  int32_t last_min = -1;
  int32_t last_batt = -1;
  int32_t last_steps = -2;
  int32_t last_goal = -2;
  bool screen_off = false;
};
S s;

// ふきだし (白い角丸 + しっぽ) — 1つの draw イベントで描く。
void draw_bubble(lv_event_t* e) {
  lv_layer_t* layer = lv_event_get_layer(e);
  const ui::Theme& t = ui::theme();

  lv_draw_rect_dsc_t r;
  lv_draw_rect_dsc_init(&r);
  r.bg_color = t.bubble_bg;
  r.bg_opa = LV_OPA_90;
  r.radius = 14;
  r.border_width = 0;
  // 左寄せ (右端はキャラに近づかない程度に)。
  lv_area_t a = {14, 262, 198, 392};  // 挨拶+歩数 (最大3行) が入る高さ
  lv_draw_rect(layer, &r, &a);

  // しっぽ (右下向きの小さな三角)。
  lv_draw_triangle_dsc_t tri;
  lv_draw_triangle_dsc_init(&tri);
  tri.color = t.bubble_bg;
  tri.opa = LV_OPA_90;
  tri.p[0].x = 64;  tri.p[0].y = 392;
  tri.p[1].x = 50;  tri.p[1].y = 408;
  tri.p[2].x = 50;  tri.p[2].y = 392;
  lv_draw_triangle(layer, &tri);
}

// 画像の最初の不透明行 (透過マージンを除いた実際の頭頂) を探す。
int32_t first_opaque_row(const lv_image_dsc_t* d) {
  const int32_t w = d->header.w;
  const int32_t h = d->header.h;
  if (d->header.cf == LV_COLOR_FORMAT_RGB565A8) {
    // RGB565A8: RGB565平面 (w*h*2) の後ろに A8平面 (w*h)。
    const uint8_t* a = d->data + static_cast<size_t>(w) * h * 2;
    for (int32_t y = 0; y < h; ++y) {
      const uint8_t* row = a + static_cast<size_t>(y) * w;
      for (int32_t x = 0; x < w; ++x) {
        if (row[x] > 16) return y;
      }
    }
    return h;
  }
  return 0;  // 透過チャンネルなし = 全面不透明とみなす
}

// 星 (時刻とふきだしの間の空き領域に小さな点4個。
// ふきだし・文字の領域には置かない)。
void draw_stars(lv_event_t* e) {
  lv_layer_t* layer = lv_event_get_layer(e);
  const ui::Theme& t = ui::theme();
  const struct { int x, y, r; } st[4] = {
      {36, 222, 2}, {130, 232, 2}, {80, 246, 2}, {180, 214, 2}};
  lv_draw_arc_dsc_t d;
  lv_draw_arc_dsc_init(&d);
  d.color = t.accent2;
  d.opa = LV_OPA_COVER;
  d.start_angle = 0;
  d.end_angle = 360;
  for (const auto& p : st) {
    d.center.x = p.x;
    d.center.y = p.y;
    d.radius = p.r;
    d.width = p.r;
    lv_draw_arc(layer, &d);
  }
}

// ふきだし文言: 挨拶 (theme.bubble[0..3]) + 歩数残り (bubble[4] 中の {n})。
void refresh_bubble() {
  const ui::Theme& t = ui::theme();
  const ui::face_data::Snapshot d = ui::face_data::get();
  const Now n = ui::face::now();
  const int hour = n.hour;
  int bucket;
  if (hour >= 5 && hour < 10) bucket = 0;        // 朝
  else if (hour >= 10 && hour < 16) bucket = 1;  // 昼
  else if (hour >= 16 && hour < 19) bucket = 2;  // 夕
  else bucket = 3;                               // 夜

  const char* greet = t.bubble[bucket];
  char text[128];
  text[0] = '\0';
  std::strncat(text, greet ? greet : "", sizeof(text) - 1);

  // 歩数残りがあるときだけ2行目に bubble[4] ({n} に残りを埋める)。
  const bool same = d.steps == s.last_steps && d.steps_goal == s.last_goal;
  if (!same) {
    s.last_steps = d.steps;
    s.last_goal = d.steps_goal;
    if (d.steps >= 0 && d.steps_goal > 0) {
      const int32_t remain = d.steps_goal - d.steps;
      if (remain > 0 && t.bubble[4]) {
        const char* tmpl = t.bubble[4];
        char line[64];
        line[0] = '\0';
        char st[12];
        ui::face::fmt_steps(st, sizeof(st), remain);
        // "{n}" を1か所だけ置換。無ければそのまま。
        const char* at = std::strstr(tmpl, "{n}");
        if (at) {
          std::snprintf(line, sizeof(line), "%.*s%s%s",
                        static_cast<int>(at - tmpl), tmpl, st, at + 3);
        } else {
          std::snprintf(line, sizeof(line), "%s", tmpl);
        }
        if (text[0]) std::strncat(text, "\n", sizeof(text) - std::strlen(text) - 1);
        std::strncat(text, line, sizeof(text) - std::strlen(text) - 1);
      }
    }
    lv_label_set_text(s.bubble_l, text);
  }
}

void refresh_data() {
  const int32_t batt = ui::port::battery_percent();
  if (batt != s.last_batt) {
    s.last_batt = batt;
    char b[8];
    ui::face::fmt_battery(b, sizeof(b), batt);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "電池 %s%%", b);
    lv_label_set_text(s.batt, buf);
  }
  refresh_bubble();
}

void tick_time() {
  const Now n = ui::face::now();
  if (n.min_of_day == s.last_min) {
    return;
  }
  s.last_min = n.min_of_day;
  char buf[24];
  ui::face::fmt_hm(buf, sizeof(buf), n);
  lv_label_set_text(s.time, buf);
  ui::face::fmt_date_jp(buf, sizeof(buf), n);
  lv_label_set_text(s.date, buf);
  // 時間帯が変われば挨拶も変わる (その場合だけ再評価したいので
  // 分境界の更新とは別に bubble も毎分確認は重い → last_goal を
  // 変えない範囲で時間帯境界のみ判定)。
  static int last_bucket = -1;
  const int hour = n.hour;
  const int bucket = (hour >= 5 && hour < 10)    ? 0
                     : (hour >= 10 && hour < 16) ? 1
                     : (hour >= 16 && hour < 19) ? 2
                                                 : 3;
  if (bucket != last_bucket) {
    last_bucket = bucket;
    s.last_goal = -2;  // 挨拶行を必ず作り直す
    refresh_bubble();
  }
}

lv_obj_t* build(lv_obj_t* scr) {
  const ui::Theme& t = ui::theme();
  s = S{};

  // 立ち絵判定。
  const lv_image_dsc_t* img = t.img_face_chara ? t.img_face_chara
                                             : t.img_stand;
  s.has_img = (img != nullptr);

  if (s.has_img) {
    // 背景は単色 (下部グラデも横縞が残るので全面単色)。
    lv_obj_set_style_bg_color(scr, t.bg, 0);

    // 星は背景レイヤー (キャラより後ろ)。ふきだし・文字の領域には置かない。
    lv_obj_t* sky = lv_obj_create(scr);
    lv_obj_set_size(sky, 410, 502);
    lv_obj_set_pos(sky, 0, 0);
    lv_obj_set_style_bg_opa(sky, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sky, 0, 0);
    lv_obj_remove_flag(sky, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sky, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(sky, draw_stars, LV_EVENT_DRAW_MAIN, nullptr);

    // 立ち絵 (右下、原寸)。基本は下端合わせだが、頭頂 (不透明先頭行)
    // が日付 (下端~196) にかかるなら画像を下げる。足元がはみ出る分は
    // 見本どおり下辺で切れるだけなので許容。
    lv_obj_t* im = lv_image_create(scr);
    lv_image_set_src(im, img);
    const int32_t ctop = first_opaque_row(img);
    int32_t im_y = 496 - static_cast<int32_t>(img->header.h);
    constexpr int32_t kHeadMinY = 208;
    if (im_y + ctop < kHeadMinY) im_y = kHeadMinY - ctop;
    lv_obj_set_pos(im, 410 - 6 - static_cast<int32_t>(img->header.w), im_y);
    lv_obj_add_flag(im, LV_OBJ_FLAG_EVENT_BUBBLE);

    // ふきだしは draw オブジェクト (キャラより手前を保証)。
    lv_obj_t* deco = lv_obj_create(scr);
    lv_obj_set_size(deco, 410, 502);
    lv_obj_set_pos(deco, 0, 0);
    lv_obj_set_style_bg_opa(deco, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(deco, 0, 0);
    lv_obj_remove_flag(deco, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(deco, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(deco, draw_bubble, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_move_foreground(deco);
  } else {
    lv_obj_set_style_bg_color(scr, t.bg, 0);
  }

  // 上: 時刻 / 日付 (中央)。
  s.time = ui::face::label(scr, "00:00", ui::face::digits(112), t.text);
  lv_obj_align(s.time, LV_ALIGN_TOP_MID, 0, 34);
  lv_obj_set_style_text_letter_space(s.time, -2, 0);
  s.date = ui::face::label(scr, "", t.font_body, t.text_dim);
  lv_obj_align(s.date, LV_ALIGN_TOP_MID, 0, 170);

  if (s.has_img) {
    // ふきだしの中身 (deco の上に別 label、幅を限定して折り返し)。
    s.bubble_l = ui::face::label(scr, "", t.font_body, t.bubble_text);
    lv_obj_set_width(s.bubble_l, 166);
    lv_label_set_long_mode(s.bubble_l, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s.bubble_l, 24, 272);
  } else {
    s.bubble_l = lv_label_create(scr);
    lv_obj_add_flag(s.bubble_l, LV_OBJ_FLAG_HIDDEN);
    // 簡易レイアウト: 時刻を中央へ。
    lv_obj_align(s.time, LV_ALIGN_CENTER, 0, -30);
    lv_obj_align(s.date, LV_ALIGN_CENTER, 0, 50);
  }

  // 下: 電池。
  s.batt = ui::face::label(scr, "", t.font_body, t.text_dim);
  if (s.has_img) {
    lv_obj_align(s.batt, LV_ALIGN_TOP_LEFT, 30, 420);
  } else {
    lv_obj_align(s.batt, LV_ALIGN_BOTTOM_MID, 0, -20);
  }

  tick_time();
  s.last_goal = -2;
  refresh_data();
  return scr;
}

void on_event(const watch::Event& e) {
  switch (e.type) {
    case watch::EventType::ClockTick:
      if (!s.screen_off) {
        tick_time();
        refresh_data();
      }
      break;
    case watch::EventType::BatteryChanged:
    case watch::EventType::ChargingChanged:
    case watch::EventType::BleConnChanged:
      if (!s.screen_off) refresh_data();
      break;
    case watch::EventType::PowerStateChanged:
      s.screen_off = e.arg0 >=
                     static_cast<uint32_t>(watch::PowerState::ScreenOff);
      if (!s.screen_off) {
        s.last_min = -1;
        s.last_goal = -2;
        tick_time();
        refresh_data();
      }
      break;
    default:
      break;
  }
}

}  // namespace

extern const ui::face::Ops kFaceOpsCharaBubble = {"chara_bubble", build, on_event};
