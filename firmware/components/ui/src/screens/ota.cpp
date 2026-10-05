// ota.cpp — ファーム更新: 現在バージョンと更新進捗の表示。
// 更新はスマホアプリから始まる (画面は進捗を見せるだけで開始ボタンは無い)。
#include "../components.hpp"
#include "../theme.hpp"
#include "screens.hpp"
#include "ui/port.hpp"
#include "ui/ui.hpp"

#include <cstdio>

namespace {

lv_obj_t* s_ver = nullptr;
lv_obj_t* s_stage = nullptr;
lv_obj_t* s_pct = nullptr;
lv_obj_t* s_bar = nullptr;
lv_obj_t* s_msg = nullptr;

// stage の日本語表示 (protocol-v1.md の stage と同じ順)。
const char* stage_jp(int stage) {
  switch (stage) {
    case 1: return "Wi-Fi接続中";
    case 2: return "受信中";
    case 3: return "検証・書き込み中";
    case 4: return "完了 (再起動します)";
    case 5: return "再起動中";
    case 6: return "失敗";
    default: return "待機中";
  }
}

void refresh() {
  if (!s_stage) return;
  ui::port::OtaView v;
  ui::port::ota_status(&v);
  lv_label_set_text(s_stage, stage_jp(v.stage));
  if (v.version[0]) {
    lv_label_set_text_fmt(s_pct, "v%s / %d%%", v.version, v.pct);
  } else {
    lv_label_set_text_fmt(s_pct, "%d%%", v.pct);
  }
  lv_bar_set_value(s_bar, v.pct, LV_ANIM_OFF);
  lv_label_set_text(s_msg, v.msg);
}

lv_obj_t* build(lv_obj_t* scr) {
  const ui::Theme& t = ui::theme();
  lv_obj_set_style_bg_color(scr, t.bg, 0);
  ui::c::header(scr, "ファーム更新", true);
  lv_obj_t* col = ui::c::content(scr);

  lv_obj_t* cur = ui::c::card(col);
  ui::c::line(cur, "現在のバージョン");
  char ver[48];
  std::snprintf(ver, sizeof(ver), "v%s", ui::port::fw_version());
  s_ver = ui::c::line(cur, ver);

  lv_obj_t* st = ui::c::card(col);
  ui::c::line(st, "更新の進捗");
  s_stage = ui::c::line(st, "待機中");
  s_bar = lv_bar_create(st);
  lv_bar_set_range(s_bar, 0, 100);
  lv_obj_set_size(s_bar, LV_PCT(100), 14);
  lv_obj_set_style_bg_color(s_bar, t.surface2, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_bar, t.primary, LV_PART_INDICATOR);
  s_pct = ui::c::line(st, "0%");
  s_msg = ui::c::line(st, "");

  lv_obj_t* hint = ui::c::card(col);
  ui::c::line(hint, "更新はスマホアプリから");
  ui::c::line(hint, "始めます (Wi-Fi または BLE)");

  refresh();
  return scr;
}

void on_event(lv_obj_t*, const watch::Event& e) {
  if (e.type == watch::EventType::OtaProgress) refresh();
}

}  // namespace

namespace ui {
extern const ScreenOps kOtaScreen = {watch::Route::Ota, build, on_event};
}
