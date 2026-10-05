// memo.cpp — メモ: 一覧と詳細。作成はスマホ経由のみ
// (時計側は閲覧・削除だけ)。plan.md メモ入力=Phone の仕様。
#include "../components.hpp"
#include "../theme.hpp"
#include "screens.hpp"
#include "ui/ui.hpp"
#include "watch/features/memo.hpp"

namespace {

struct M {
  lv_obj_t* list_box = nullptr;
  lv_obj_t* detail_box = nullptr;
  lv_obj_t* detail_l = nullptr;
  uint32_t detail_id = 0;
};
M s;

void show_list() {
  lv_obj_remove_flag(s.list_box, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s.detail_box, LV_OBJ_FLAG_HIDDEN);
}

void rebuild_list() {
  lv_obj_clean(s.list_box);
  const size_t n = watch::features::memo_count();
  if (n == 0) {
    ui::c::line(s.list_box, "メモはまだありません");
    ui::c::line(s.list_box, "スマホアプリから追加できます");
    return;
  }
  // 新しいものを上に (memo_at は古い順)。
  for (size_t i = n; i > 0; --i) {
    watch::features::MemoEntry m{};
    if (!watch::features::memo_at(i - 1, &m)) continue;
    ui::c::list_row(s.list_box, m.text, nullptr,
                    [](lv_event_t* e) {
                      s.detail_id = static_cast<uint32_t>(
                          reinterpret_cast<uintptr_t>(
                              lv_event_get_user_data(e)));
                      watch::features::MemoEntry m{};
                      if (watch::features::memo_find(s.detail_id, &m)) {
                        lv_label_set_text(s.detail_l, m.text);
                        lv_obj_add_flag(s.list_box, LV_OBJ_FLAG_HIDDEN);
                        lv_obj_remove_flag(s.detail_box,
                                           LV_OBJ_FLAG_HIDDEN);
                      }
                    },
                    reinterpret_cast<void*>(static_cast<uintptr_t>(m.id)));
  }
}

lv_obj_t* build(lv_obj_t* scr) {
  const ui::Theme& t = ui::theme();
  lv_obj_set_style_bg_color(scr, t.bg, 0);
  ui::c::header(scr, "メモ", true);
  lv_obj_t* col = ui::c::content(scr);
  s = M{};

  s.list_box = ui::c::card(col);

  s.detail_box = ui::c::card(col);
  lv_obj_set_flex_align(s.detail_box, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  s.detail_l = lv_label_create(s.detail_box);
  lv_obj_set_width(s.detail_l, 330);
  lv_obj_set_style_text_font(s.detail_l, t.font_body, 0);
  lv_obj_set_style_text_color(s.detail_l, t.text, 0);
  lv_label_set_long_mode(s.detail_l, LV_LABEL_LONG_WRAP);

  ui::c::button_danger(
      s.detail_box, "削除",
      [](lv_event_t*) {
        ui::emit(watch::ActionType::MemoDelete, s.detail_id);
        show_list();
      },
      nullptr);
  ui::c::button(s.detail_box, "一覧に戻る",
                [](lv_event_t*) { show_list(); }, nullptr);
  lv_obj_add_flag(s.detail_box, LV_OBJ_FLAG_HIDDEN);

  rebuild_list();
  return scr;
}

void on_event(lv_obj_t*, const watch::Event& e) {
  if (e.type == watch::EventType::MemoSaved ||
      e.type == watch::EventType::MemoDeleted) {
    rebuild_list();
    show_list();
  }
}

}  // namespace

namespace ui {
extern const ScreenOps kMemoScreen = {watch::Route::Memo, build, on_event};
}
