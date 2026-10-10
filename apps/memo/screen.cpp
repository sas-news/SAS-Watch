// memo.cpp — メモ: 録音・一覧・詳細 (テキスト/音声)。
// テキストメモの作成はスマホ経由のみ (plan.md メモ入力=Phone の仕様)。
// 音声メモは時計側で録音でき、実体は AudioPort 側のストレージに置く。
#include "components.hpp"
#include "theme.hpp"
#include "screens/screens.hpp"
#include "ui/ui.hpp"
#include "watch/features/memo.hpp"

#include <cstdio>

namespace {

struct M {
  lv_obj_t* rec_card = nullptr;
  lv_obj_t* rec_btn = nullptr;
  lv_obj_t* rec_info = nullptr;
  lv_obj_t* rec_bar = nullptr;
  lv_obj_t* list_box = nullptr;
  lv_obj_t* detail_box = nullptr;
  lv_obj_t* detail_l = nullptr;
  lv_obj_t* play_btn = nullptr;
  uint32_t detail_id = 0;
  bool detail_voice = false;
};
M s;

watch::FeatureContext* fctx() { return ui::ctx().fctx; }

lv_obj_t* fit_btn(lv_obj_t* card, const char* text, lv_event_cb_t cb) {
  return ui::c::button_primary(card, text, cb, nullptr);
}

void set_btn_text(lv_obj_t* btn, const char* text) {
  lv_obj_t* l = lv_obj_get_child(btn, 0);
  if (l) lv_label_set_text(l, text);
}

void refresh_rec() {
  if (!s.rec_card) return;
  const bool rec = watch::features::memo_recording();
  set_btn_text(s.rec_btn, rec ? "停止" : "録音開始");
  if (rec && fctx()) {
    lv_obj_remove_flag(s.rec_info, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s.rec_bar, LV_OBJ_FLAG_HIDDEN);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "録音中 %lu 秒",
                  static_cast<unsigned long>(
                      watch::features::memo_record_elapsed_s(*fctx())));
    lv_label_set_text(s.rec_info, buf);
    lv_bar_set_value(s.rec_bar,
                     watch::features::memo_record_level(*fctx()),
                     LV_ANIM_OFF);
  } else {
    lv_obj_add_flag(s.rec_info, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s.rec_bar, LV_OBJ_FLAG_HIDDEN);
  }
}

void refresh_play() {
  if (!s.play_btn) return;
  const bool playing =
      s.detail_voice &&
      watch::features::memo_playing_id() == s.detail_id;
  set_btn_text(s.play_btn, playing ? "停止" : "再生");
}

void show_list() {
  lv_obj_remove_flag(s.list_box, LV_OBJ_FLAG_HIDDEN);
  if (s.rec_card) lv_obj_remove_flag(s.rec_card, LV_OBJ_FLAG_HIDDEN);
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
    char label[64];
    if (m.kind == watch::features::MemoKind::Voice) {
      std::snprintf(label, sizeof(label), "音声メモ %lu 秒",
                    static_cast<unsigned long>(m.sec));
    } else {
      std::snprintf(label, sizeof(label), "%s", m.text);
    }
    ui::c::row(s.list_box, label, nullptr, nullptr, true,
                    [](lv_event_t* e) {
                      s.detail_id = static_cast<uint32_t>(
                          reinterpret_cast<uintptr_t>(
                              lv_event_get_user_data(e)));
                      watch::features::MemoEntry m{};
                      if (!watch::features::memo_find(s.detail_id, &m)) {
                        return;
                      }
                      s.detail_voice =
                          m.kind == watch::features::MemoKind::Voice;
                      if (s.detail_voice) {
                        char buf[48];
                        std::snprintf(buf, sizeof(buf), "音声メモ %lu 秒",
                                      static_cast<unsigned long>(m.sec));
                        lv_label_set_text(s.detail_l, buf);
                        lv_obj_remove_flag(s.play_btn, LV_OBJ_FLAG_HIDDEN);
                        refresh_play();
                      } else {
                        lv_label_set_text(s.detail_l, m.text);
                        lv_obj_add_flag(s.play_btn, LV_OBJ_FLAG_HIDDEN);
                      }
                      lv_obj_add_flag(s.list_box, LV_OBJ_FLAG_HIDDEN);
                      if (s.rec_card) {
                        lv_obj_add_flag(s.rec_card, LV_OBJ_FLAG_HIDDEN);
                      }
                      lv_obj_remove_flag(s.detail_box,
                                         LV_OBJ_FLAG_HIDDEN);
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

  // 録音ブロックは AudioPort がある時だけ (スピーカー無し機種もあり得る)。
  if (fctx() && fctx()->audio) {
    s.rec_card = ui::c::card(col);
    s.rec_btn = fit_btn(s.rec_card, "録音開始",
        [](lv_event_t*) {
          if (watch::features::memo_recording()) {
            ui::emit(watch::ActionType::MemoRecordStop);
          } else {
            ui::emit(watch::ActionType::MemoRecordStart);
          }
        });
    s.rec_info = ui::c::line(s.rec_card, "録音中 0 秒");
    s.rec_bar = lv_bar_create(s.rec_card);
    lv_bar_set_range(s.rec_bar, 0, 100);
    lv_obj_set_size(s.rec_bar, LV_PCT(100), 14);
    lv_obj_set_style_bg_color(s.rec_bar, t.surface2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s.rec_bar, t.primary, LV_PART_INDICATOR);
    refresh_rec();
  }

  s.list_box = ui::c::card(col);

  s.detail_box = ui::c::card(col);
  lv_obj_set_flex_align(s.detail_box, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  s.detail_l = lv_label_create(s.detail_box);
  lv_obj_set_width(s.detail_l, LV_PCT(100));
  lv_obj_set_style_text_font(s.detail_l, t.font_body, 0);
  lv_obj_set_style_text_color(s.detail_l, t.text, 0);
  lv_label_set_long_mode(s.detail_l, LV_LABEL_LONG_WRAP);

  s.play_btn = fit_btn(s.detail_box, "再生",
      [](lv_event_t*) {
        if (watch::features::memo_playing_id() == s.detail_id) {
          ui::emit(watch::ActionType::MemoStopPlay);
        } else {
          ui::emit(watch::ActionType::MemoPlay, s.detail_id);
        }
      });

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
  refresh_rec();
  return scr;
}

void on_event(lv_obj_t*, const watch::Event& e) {
  if (e.type == watch::EventType::ClockTick) {
    refresh_rec();
    refresh_play();
    return;
  }
  if (e.type == watch::EventType::MemoSaved ||
      e.type == watch::EventType::MemoDeleted) {
    rebuild_list();
    refresh_rec();
    show_list();
  }
}

}  // namespace

namespace ui {
extern const ScreenOps kMemoScreen = {watch::Route::Memo, build, on_event};
}
