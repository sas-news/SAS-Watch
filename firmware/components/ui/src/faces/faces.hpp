// faces.hpp — 文字盤 (watch face) の内部インターフェース。
//   screens/home.cpp はここを通して現在の文字盤に組み立て・Event を委譲する。
//   各文字盤は faces/face_*.cpp に Ops を1個ずつ定義する。
//   文字盤が使う色はすべて ui::theme() のトークン、日本語ラベルは
//   ui::theme().font_body/font_title (独自フォントは時計数字だけ)。
#pragma once

#include "../theme.hpp"
#include "ui/ui.hpp"

namespace ui::face {

// 文字盤1個分。id は settings.face / protocol-v1.md の face 値と一致。
struct Ops {
  const char* id;
  lv_obj_t* (*build)(lv_obj_t* scr);
  void (*on_event)(const watch::Event& e);
};

// id → Ops。未知・空なら "bold" の Ops を返す (フォールバック)。
const Ops* find(const char* id);
// settings.face から現在の Ops。
const Ops* current();
// 全部の文字盤 (設定画面の一覧用)。終端は id=nullptr。
const Ops* const* all();
// id → 日本語表示名 (設定画面用)。
const char* label_ja(const char* id);

// ---- 共有ユーティリティ -------------------------------------------------

// 現在時刻 (TZ 考慮)。ClockTick 未着の起動直後も clock feature の値を使う。
struct Now {
  int hour = 0;
  int min = 0;
  int sec = 0;
  int mday = 0;
  int mon = 0;   // 1-12
  int year = 0;  // 4桁
  int wday = 0;  // 0=日
  int min_of_day = 0;  // 0-1439 (アラーム比較・日進捗用)
};
Now now();

void fmt_hm(char* buf, size_t cap, const Now& n);       // "18:41"
void fmt_date_jp(char* buf, size_t cap, const Now& n);  // "10月5日 日曜日"
void fmt_date_md(char* buf, size_t cap, const Now& n);  // "10/5"
const char* wd_jp(int wday);  // "日".."土"
const char* wd_en(int wday);  // "SUN".."SAT"

// clock_font 設定を解決した時計数字フォント。
//   px = 150 (縦積み) / 112 (横並び) / 34 (analog 数字) / 18 (小さい数値)。
//   "auto" は文字盤ごとの見本フォント。未知値は "auto" と同じ。
const lv_font_t* digits(int px);
// bold の分側など「細字 variant」。無いファミリーは digits(px) と同じ。
const lv_font_t* digits_thin(int px);
// 現在の文字盤の auto フォント family id。
const char* auto_font_id();
// clock_font 設定値を正規化した family id ("auto" や未知は nullptr ではなく
// auto側の family が要るときは current_family() を使う)。
const char* clock_font_setting();

// 3桁区切り歩数 "-1 → --"
void fmt_steps(char* buf, size_t cap, int32_t steps);

// 補助データ (ui/face_data.hpp) の使いやすい整形
//   battery% (-1→--), 電池行テキスト。
void fmt_battery(char* buf, size_t cap, int32_t pct);

// 部品 (装飾は毎画面で書くが、よくある形だけここに置く)
lv_obj_t* label(lv_obj_t* parent, const char* text, const lv_font_t* font,
                lv_color_t color);
lv_obj_t* rect(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
               lv_coord_t h, lv_color_t color, lv_coord_t radius);
// 「--」 or 数値を入れるラベル更新ヘルパ。
void set_label_i(lv_obj_t* l, int32_t v);

}  // namespace ui::face
