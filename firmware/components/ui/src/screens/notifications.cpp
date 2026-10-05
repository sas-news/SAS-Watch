// notifications.cpp — 通知一覧: 最新20件 (アプリ名/タイトル/本文1行)。
// タップで詳細、「すべて消す」でクリア。ポップアップ・振動は shell 側。
#include "../components.hpp"
#include "../theme.hpp"
#include "screens.hpp"
#include "ui/ui.hpp"
#include "watch/features/notify.hpp"

#include <cstdio>
#include <cstring>

namespace {

struct M {
  lv_obj_t* list_box = nullptr;
  lv_obj_t* detail_box = nullptr;
  lv_obj_t* detail_app = nullptr;
  lv_obj_t* detail_title = nullptr;
  lv_obj_t* detail_body = nullptr;
};
M s;

void fit_in_card(lv_obj_t* o) { lv_obj_set_width(o, LV_PCT(100)); }

void show_list() {
  lv_obj_remove_flag(s.list_box, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s.detail_box, LV_OBJ_FLAG_HIDDEN);
}

// 本文の最初の1行だけを buf に写す。
void first_line(const char* s_, char* buf, size_t cap) {
  size_t n = 0;
  while (s_[n] && s_[n] != '\n' && n + 1 < cap) {
    buf[n] = s_[n];
    ++n;
  }
  buf[n] = '\0';
}

void rebuild_list() {
  lv_obj_clean(s.list_box);
  const size_t n = watch::features::notify_count();
  if (n == 0) {
    ui::c::line(s.list_box, "通知はまだありません");
    ui::c::line(s.list_box, "スマホアプリの通知がここに届きます");
    return;
  }
  for (size_t i = 0; i < n; ++i) {
    watch::features::NotifyEntry ne;
    if (!watch::features::notify_at(i, &ne)) continue;
    char body1[80];
    first_line(ne.body, body1, sizeof(body1));
    // 行は「アプリ: タイトル」+ 右に本文1行 (DOT で省略)。
    char head[160];
    std::snprintf(head, sizeof(head), "%s: %s", ne.app, ne.title);
    lv_obj_t* row = ui::c::list_row(
        s.list_box, head, body1,
        [](lv_event_t* e) {
          const size_t i = static_cast<size_t>(
              reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
          watch::features::NotifyEntry ne;
          if (!watch::features::notify_at(i, &ne)) return;
          lv_label_set_text(s.detail_app, ne.app);
          lv_label_set_text(s.detail_title, ne.title);
          lv_label_set_text(s.detail_body, ne.body);
          lv_obj_add_flag(s.list_box, LV_OBJ_FLAG_HIDDEN);
          lv_obj_remove_flag(s.detail_box, LV_OBJ_FLAG_HIDDEN);
        },
        reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
    fit_in_card(row);
  }
}

lv_obj_t* build(lv_obj_t* scr) {
  const ui::Theme& t = ui::theme();
  lv_obj_set_style_bg_color(scr, t.bg, 0);
  ui::c::header(scr, "通知", true);
  lv_obj_t* col = ui::c::content(scr);
  s = M{};

  s.list_box = ui::c::card(col);

  s.detail_box = ui::c::card(col);
  lv_obj_set_flex_align(s.detail_box, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  s.detail_app = lv_label_create(s.detail_box);
  lv_obj_set_width(s.detail_app, 330);
  lv_obj_set_style_text_font(s.detail_app, t.font_body, 0);
  lv_obj_set_style_text_color(s.detail_app, t.text_dim, 0);
  lv_label_set_long_mode(s.detail_app, LV_LABEL_LONG_WRAP);

  s.detail_title = lv_label_create(s.detail_box);
  lv_obj_set_width(s.detail_title, 330);
  lv_obj_set_style_text_font(s.detail_title, t.font_body, 0);
  lv_obj_set_style_text_color(s.detail_title, t.text, 0);
  lv_label_set_long_mode(s.detail_title, LV_LABEL_LONG_WRAP);

  s.detail_body = lv_label_create(s.detail_box);
  lv_obj_set_width(s.detail_body, 330);
  lv_obj_set_style_text_font(s.detail_body, t.font_body, 0);
  lv_obj_set_style_text_color(s.detail_body, t.text_dim, 0);
  lv_label_set_long_mode(s.detail_body, LV_LABEL_LONG_WRAP);

  fit_in_card(ui::c::button(s.detail_box, "一覧に戻る",
                          [](lv_event_t*) { show_list(); }, nullptr));
  lv_obj_add_flag(s.detail_box, LV_OBJ_FLAG_HIDDEN);

  fit_in_card(ui::c::button_danger(
      col, "すべて消す",
      [](lv_event_t*) {
        ui::emit(watch::ActionType::NotifyClearAll);
        show_list();
      },
      nullptr));

  rebuild_list();
  return scr;
}

void on_event(lv_obj_t*, const watch::Event& e) {
  if (e.type == watch::EventType::NotificationPosted ||
      e.type == watch::EventType::NotificationsCleared) {
    if (s.list_box) rebuild_list();
    if (e.type == watch::EventType::NotificationsCleared) show_list();
  }
}

}  // namespace

namespace ui {
extern const ScreenOps kNotificationsScreen = {watch::Route::Notifications,
                                               build, on_event};
}
