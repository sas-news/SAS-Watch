// steps.cpp — 歩数: 当日歩数・目標・達成率バー。
//   部品は ui::c と theme トークンのみ (デザイン刷新と競合しないため)。
//   バーは theme 色だけで組む (surface2 のトラック + primary の塗り)。
#include "components.hpp"
#include "theme.hpp"

#include <cstdio>

#include "screens/screens.hpp"
#include "ui/ui.hpp"
#include "watch/features/steps.hpp"
#include "watch/settings.hpp"

namespace {

lv_obj_t* s_num = nullptr;
lv_obj_t* s_goal_l = nullptr;
lv_obj_t* s_rate_l = nullptr;
lv_obj_t* s_fill = nullptr;

uint32_t goal() {
  const watch::Settings* st = ui::ctx().settings;
  const uint32_t g = st ? st->steps_goal : 8000;
  return g ? g : 8000;
}

void refresh() {
  const uint32_t steps = watch::features::steps_today();
  const uint32_t g = goal();
  char buf[40];

  std::snprintf(buf, sizeof(buf), "%lu",
                static_cast<unsigned long>(steps));
  lv_label_set_text(s_num, buf);

  std::snprintf(buf, sizeof(buf), "目標 %lu 歩",
                static_cast<unsigned long>(g));
  lv_label_set_text(s_goal_l, buf);

  const uint32_t pct = steps * 100 / g;
  std::snprintf(buf, sizeof(buf), "達成率 %lu%%",
                static_cast<unsigned long>(pct));
  lv_label_set_text(s_rate_l, buf);

  const int w = static_cast<int>(pct > 100 ? 100 : pct);
  lv_obj_set_width(s_fill, lv_pct(w));
}

lv_obj_t* build(lv_obj_t* scr) {
  const ui::Theme& t = ui::theme();
  lv_obj_set_style_bg_color(scr, t.bg, 0);
  ui::c::header(scr, "歩数", true);
  lv_obj_t* col = ui::c::content(scr);

  // 当日歩数 (大きめ数字)。
  lv_obj_t* card = ui::c::card(col);
  lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  s_num = lv_label_create(card);
  lv_obj_set_style_text_font(s_num, t.font_digits_sm, 0);
  lv_obj_set_style_text_color(s_num, t.accent, 0);
  ui::c::line(card, "今日の歩数");

  // 目標 + 達成率バー。
  lv_obj_t* prog = ui::c::card(col);
  s_goal_l = ui::c::line(prog, "目標");

  lv_obj_t* track = lv_obj_create(prog);
  lv_obj_remove_style_all(track);
  lv_obj_set_size(track, lv_pct(100), 24);
  lv_obj_set_style_bg_color(track, t.surface2, 0);
  lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(track, t.radius_sm, 0);

  s_fill = lv_obj_create(track);
  lv_obj_remove_style_all(s_fill);
  lv_obj_set_size(s_fill, lv_pct(0), lv_pct(100));
  lv_obj_set_style_bg_color(s_fill, t.primary, 0);
  lv_obj_set_style_bg_opa(s_fill, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(s_fill, t.radius_sm, 0);
  lv_obj_align(s_fill, LV_ALIGN_LEFT_MID, 0, 0);

  s_rate_l = ui::c::line(prog, "達成率");

  // 画面OFF中は限定的にしか数えない (実機まで確かでない) 旨を書く。
  ui::c::line(col, "画面点灯中と起床直後に計測");

  refresh();
  return scr;
}

void on_event(lv_obj_t*, const watch::Event& e) {
  if (e.type == watch::EventType::StepsChanged ||
      e.type == watch::EventType::SettingsChanged) {
    refresh();
  }
}

}  // namespace

namespace ui {
extern const ScreenOps kStepsScreen = {watch::Route::Steps, build, on_event};
}
