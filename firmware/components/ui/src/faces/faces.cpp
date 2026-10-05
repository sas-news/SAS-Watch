// faces.cpp — 文字盤レジストリ + 共有ユーティリティ。
//   clock_font 設定のフォント解決・時刻整形・小物生成をまとめる。
#include "faces.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "ui/face_data.hpp"
#include "watch/features/clock.hpp"

// 時計数字フォント (tools/gen_fonts.py --faces で生成)。
LV_FONT_DECLARE(font_fc_oswald_150);
LV_FONT_DECLARE(font_fc_oswald_l_150);
LV_FONT_DECLARE(font_fc_oswald_112);
LV_FONT_DECLARE(font_fc_oswald_34);
LV_FONT_DECLARE(font_fc_oswald_18);
LV_FONT_DECLARE(font_fc_bebas_150);
LV_FONT_DECLARE(font_fc_bebas_112);
LV_FONT_DECLARE(font_fc_bebas_34);
LV_FONT_DECLARE(font_fc_bebas_18);
LV_FONT_DECLARE(font_fc_orbitron_150);
LV_FONT_DECLARE(font_fc_orbitron_112);
LV_FONT_DECLARE(font_fc_orbitron_34);
LV_FONT_DECLARE(font_fc_orbitron_18);
LV_FONT_DECLARE(font_fc_outfit_150);
LV_FONT_DECLARE(font_fc_outfit_112);
LV_FONT_DECLARE(font_fc_outfit_34);
LV_FONT_DECLARE(font_fc_outfit_18);
LV_FONT_DECLARE(font_fc_chakra_150);
LV_FONT_DECLARE(font_fc_chakra_112);
LV_FONT_DECLARE(font_fc_chakra_34);
LV_FONT_DECLARE(font_fc_chakra_18);

// 各文字盤の Ops (face_*.cpp で定義)。
extern const ui::face::Ops kFaceOpsBold;
extern const ui::face::Ops kFaceOpsAnalog;
extern const ui::face::Ops kFaceOpsHud;
extern const ui::face::Ops kFaceOpsMinimal;
extern const ui::face::Ops kFaceOpsCharaSide;
extern const ui::face::Ops kFaceOpsCharaBubble;

namespace ui::face {
namespace {

const Ops* const kFaces[] = {
    &kFaceOpsBold,   &kFaceOpsAnalog,      &kFaceOpsHud,
    &kFaceOpsMinimal, &kFaceOpsCharaSide,  &kFaceOpsCharaBubble,
    nullptr,
};

// 文字盤 id → auto で使うフォント family (見本どおり)。
const char* kAutoFont[] = {
    /*bold*/        "oswald",
    /*analog*/      "oswald",
    /*hud*/         "orbitron",
    /*minimal*/     "outfit",
    /*chara_side*/  "bebas",
    /*chara_bubble*/"outfit",
};

struct ClockFont {
  const char* id;
  const lv_font_t* px150;       // 縦積み大数字
  const lv_font_t* px150_thin;  // 縦積みの「細」側 (無ければ px150)
  const lv_font_t* px112;       // 横並び大数字
  const lv_font_t* px34;        // analog 数字
  const lv_font_t* px18;        // 小さい数値
};

const ClockFont kFonts[] = {
    {"oswald", &font_fc_oswald_150, &font_fc_oswald_l_150,
     &font_fc_oswald_112, &font_fc_oswald_34, &font_fc_oswald_18},
    {"bebas", &font_fc_bebas_150, nullptr,
     &font_fc_bebas_112, &font_fc_bebas_34, &font_fc_bebas_18},
    {"orbitron", &font_fc_orbitron_150, nullptr,
     &font_fc_orbitron_112, &font_fc_orbitron_34, &font_fc_orbitron_18},
    {"outfit", &font_fc_outfit_150, nullptr,
     &font_fc_outfit_112, &font_fc_outfit_34, &font_fc_outfit_18},
    {"chakra", &font_fc_chakra_150, nullptr,
     &font_fc_chakra_112, &font_fc_chakra_34, &font_fc_chakra_18},
};

const ClockFont* font_by_id(const char* id) {
  for (const ClockFont& f : kFonts) {
    if (std::strcmp(f.id, id) == 0) return &f;
  }
  return nullptr;
}

const ClockFont* current_family() {
  const watch::Settings* st = ctx().settings;
  const char* sel = st ? st->clock_font : "auto";
  const ClockFont* f = font_by_id(sel);
  if (f) return f;
  // "auto" / 未知 → 現在の文字盤の見本フォント。
  return font_by_id(auto_font_id());
}

const lv_font_t* at(const ClockFont* f, int px, bool thin) {
  switch (px) {
    case 150: return (thin && f->px150_thin) ? f->px150_thin : f->px150;
    case 112: return f->px112;
    case 34: return f->px34;
    case 18: return f->px18;
    default: return f->px112;
  }
}

struct { const char* id; const char* ja; } kFaceJa[] = {
    {"bold", "ボールド"},       {"analog", "アナログ"},
    {"hud", "HUD"},             {"minimal", "ミニマル"},
    {"chara_side", "キャラ横"}, {"chara_bubble", "キャラふきだし"},
};

}  // namespace

const Ops* find(const char* id) {
  for (const Ops* const* p = kFaces; *p; ++p) {
    if (id && std::strcmp((*p)->id, id) == 0) return *p;
  }
  return kFaces[0];  // 未知 → bold
}

const Ops* current() {
  const watch::Settings* st = ctx().settings;
  return find(st ? st->face : "bold");
}

const Ops* const* all() { return kFaces; }

const char* label_ja(const char* id) {
  for (const auto& m : kFaceJa) {
    if (std::strcmp(m.id, id) == 0) return m.ja;
  }
  return id;
}

const char* auto_font_id() {
  const Ops* cur = current();
  for (size_t i = 0; i < sizeof(kFaces) / sizeof(kFaces[0]) - 1; ++i) {
    if (kFaces[i] == cur) return kAutoFont[i];
  }
  return kAutoFont[0];
}

const char* clock_font_setting() {
  const watch::Settings* st = ctx().settings;
  return st ? st->clock_font : "auto";
}

const lv_font_t* digits(int px) { return at(current_family(), px, false); }
const lv_font_t* digits_thin(int px) { return at(current_family(), px, true); }

Now now() {
  const watch::Settings* st = ctx().settings;
  const int32_t tz = st ? st->tz_offset_min : 0;
  const int64_t epoch = watch::features::clock_last_epoch_s();
  const time_t t = static_cast<time_t>(epoch + static_cast<int64_t>(tz) * 60);
  struct tm tmv {};
  gmtime_r(&t, &tmv);
  Now n;
  n.hour = tmv.tm_hour;
  n.min = tmv.tm_min;
  n.sec = tmv.tm_sec;
  n.mday = tmv.tm_mday;
  n.mon = tmv.tm_mon + 1;
  n.year = tmv.tm_year + 1900;
  n.wday = tmv.tm_wday;
  n.min_of_day = tmv.tm_hour * 60 + tmv.tm_min;
  return n;
}

void fmt_hm(char* buf, size_t cap, const Now& n) {
  std::snprintf(buf, cap, "%02d:%02d", n.hour, n.min);
}
void fmt_date_jp(char* buf, size_t cap, const Now& n) {
  std::snprintf(buf, cap, "%d月%d日 %s曜日", n.mon, n.mday, wd_jp(n.wday));
}
void fmt_date_md(char* buf, size_t cap, const Now& n) {
  std::snprintf(buf, cap, "%d/%d", n.mon, n.mday);
}

const char* wd_jp(int wday) {
  static const char* k[] = {"日", "月", "火", "水", "木", "金", "土"};
  return (wday >= 0 && wday <= 6) ? k[wday] : "?";
}
const char* wd_en(int wday) {
  static const char* k[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  return (wday >= 0 && wday <= 6) ? k[wday] : "???";
}

void fmt_steps(char* buf, size_t cap, int32_t steps) {
  if (steps < 0) {
    std::snprintf(buf, cap, "--");
  } else if (steps < 1000) {
    std::snprintf(buf, cap, "%d", static_cast<int>(steps));
  } else {
    std::snprintf(buf, cap, "%d,%03d", static_cast<int>(steps / 1000),
                  static_cast<int>(steps % 1000));
  }
}

void fmt_battery(char* buf, size_t cap, int32_t pct) {
  if (pct < 0) {
    std::snprintf(buf, cap, "--");
  } else {
    std::snprintf(buf, cap, "%d", static_cast<int>(pct));
  }
}

lv_obj_t* label(lv_obj_t* parent, const char* text, const lv_font_t* font,
                lv_color_t color) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_label_set_text(l, text);
  lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
  return l;
}

lv_obj_t* rect(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
               lv_coord_t h, lv_color_t color, lv_coord_t radius) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, color, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
  return o;
}

void set_label_i(lv_obj_t* l, int32_t v) {
  char buf[12];
  if (v < 0) {
    lv_label_set_text(l, "--");
    return;
  }
  std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(v));
  lv_label_set_text(l, buf);
}

}  // namespace ui::face
