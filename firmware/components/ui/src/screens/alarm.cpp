// alarm.cpp — アラーム: 大きな時刻 + 曜日 sub + スイッチの行。
// 追加・編集はスマホアプリ (alarm.list/alarm.set/alarm.delete) から。
// 行タップで有効/無効を切替え、鳴動中は shell のアラートが前面に出る。
#include "../components.hpp"
#include "../faces/faces.hpp"
#include "../theme.hpp"
#include "screens.hpp"
#include "ui/ui.hpp"
#include "watch/features/alarm.hpp"

#include <cstdio>

namespace {

lv_obj_t* s_grp = nullptr;

// dow マスク → "毎日" / "日月火..." の短い表記。
void fmt_dow(uint8_t dow, char* buf, size_t cap) {
  if (dow == 0) {
    std::snprintf(buf, cap, "毎日");
    return;
  }
  static const char* names[7] = {"日", "月", "火", "水", "木", "金", "土"};
  size_t n = 0;
  for (int i = 0; i < 7 && n + 4 < cap; ++i) {
    if (dow & (1u << i)) {
      n += static_cast<size_t>(
          std::snprintf(buf + n, cap - n, "%s", names[i]));
    }
  }
  buf[n] = '\0';
}

void rebuild_list() {
  lv_obj_clean(s_grp);
  const ui::Theme& t = ui::theme();
  const size_t n = watch::features::alarm_count();
  if (n == 0) {
    ui::c::line(s_grp, "アラームはまだありません");
    return;
  }
  for (size_t i = 0; i < n; ++i) {
    watch::features::AlarmEntry a;
    if (!watch::features::alarm_at(i, &a)) continue;
    char time_s[8];
    std::snprintf(time_s, sizeof(time_s), "%u:%02u", a.hour, a.min);
    char dow_s[32];
    fmt_dow(a.dow, dow_s, sizeof(dow_s));

    lv_obj_t* r = ui::c::row_box(
        s_grp,
        [](lv_event_t* e) {
          const uint32_t id = static_cast<uint32_t>(
              reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
          ui::emit(watch::ActionType::AlarmToggle, id);
        },
        reinterpret_cast<void*>(static_cast<uintptr_t>(a.id)));
    lv_obj_set_style_min_height(r, 80, 0);

    // 時刻は時計フォント (34px)、曜日は本文フォントの薄い sub。
    lv_obj_t* cell = lv_obj_create(r);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(cell, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_min_width(cell, 60, 0);
    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cell, 0, 0);
    lv_obj_set_style_pad_all(cell, 0, 0);
    // 数字フォントと本文フォントの行高が大きいので負値で曜日行を近づける。
    lv_obj_set_style_pad_row(cell, -10, 0);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(cell, 1);
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* tl = lv_label_create(cell);
    lv_obj_add_flag(tl, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_label_set_text(tl, time_s);
    lv_obj_set_style_text_font(tl, ui::face::digits(34), 0);
    lv_obj_set_style_text_color(tl, t.text, 0);
    lv_obj_t* dl = lv_label_create(cell);
    lv_obj_add_flag(dl, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_label_set_text(dl, dow_s);
    lv_obj_set_style_text_font(dl, t.font_body, 0);
    lv_obj_set_style_text_color(dl, t.text_dim, 0);

    ui::c::mk_switch(r, a.enabled);

    // 無効アラームは行ごと薄くする (モックの opacity:.55)。
    if (!a.enabled) lv_obj_set_style_opa(r, 140, 0);
  }
}

lv_obj_t* build(lv_obj_t* scr) {
  const ui::Theme& t = ui::theme();
  lv_obj_set_style_bg_color(scr, t.bg, 0);
  ui::c::header(scr, "アラーム", true);
  lv_obj_t* col = ui::c::content(scr);

  s_grp = ui::c::group(col);
  lv_obj_t* hint = lv_label_create(col);
  lv_obj_add_flag(hint, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_label_set_text(hint, "追加・編集はスマホアプリから");
  lv_obj_set_style_text_font(hint, t.font_body, 0);
  lv_obj_set_style_text_color(hint, t.text_dim, 0);
  lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(hint, LV_PCT(100));
  rebuild_list();
  return scr;
}

void on_event(lv_obj_t*, const watch::Event& e) {
  if (e.type == watch::EventType::AlarmChanged && s_grp) rebuild_list();
}

}  // namespace

namespace ui {
extern const ScreenOps kAlarmScreen = {watch::Route::Alarm, build, on_event};
}
