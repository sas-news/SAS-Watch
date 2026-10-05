// media.cpp — 音楽操作: 曲名・アーティスト・再生状態 + 縦並びの操作ボタン。
// ボタン → MediaCommand Action → MediaCmdRequested Event → EVT "media.cmd"。
#include "../components.hpp"
#include "../theme.hpp"
#include "screens.hpp"
#include "ui/ui.hpp"
#include "watch/features/media.hpp"

#include <cstdio>

namespace {

struct M {
  lv_obj_t* title = nullptr;
  lv_obj_t* artist = nullptr;
  lv_obj_t* state = nullptr;
};
M s;

void refresh() {
  if (!s.title) return;
  const watch::features::MediaState& m = watch::features::media_state();
  if (!m.valid) {
    lv_label_set_text(s.title, "再生情報がありません");
    lv_label_set_text(s.artist, "スマホで音楽を再生するとここに出ます");
    lv_label_set_text(s.state, "");
    return;
  }
  lv_label_set_text(s.title, m.title[0] ? m.title : "(曲名なし)");
  lv_label_set_text(s.artist, m.artist);
  lv_label_set_text(s.state, m.playing ? "再生中" : "一時停止中");
}

void cmd_btn(lv_obj_t* card, const char* text, watch::MediaCmd cmd) {
  ui::c::button(
      card, text,
      [](lv_event_t* e) {
        const auto c = static_cast<watch::MediaCmd>(
            reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
        ui::emit(watch::ActionType::MediaCommand, static_cast<uint32_t>(c));
      },
      reinterpret_cast<void*>(static_cast<uintptr_t>(cmd)));
}

lv_obj_t* build(lv_obj_t* scr) {
  const ui::Theme& t = ui::theme();
  lv_obj_set_style_bg_color(scr, t.bg, 0);
  ui::c::header(scr, "音楽", true);
  lv_obj_t* col = ui::c::content(scr);
  s = M{};

  lv_obj_t* info = ui::c::card(col);
  s.state = lv_label_create(info);
  lv_obj_set_style_text_font(s.state, t.font_body, 0);
  lv_obj_set_style_text_color(s.state, t.accent, 0);
  s.title = lv_label_create(info);
  lv_obj_set_width(s.title, LV_PCT(100));
  lv_obj_set_style_text_font(s.title, t.font_body, 0);
  lv_obj_set_style_text_color(s.title, t.text, 0);
  lv_label_set_long_mode(s.title, LV_LABEL_LONG_DOT);
  s.artist = lv_label_create(info);
  lv_obj_set_width(s.artist, LV_PCT(100));
  lv_obj_set_style_text_font(s.artist, t.font_body, 0);
  lv_obj_set_style_text_color(s.artist, t.text_dim, 0);
  lv_label_set_long_mode(s.artist, LV_LABEL_LONG_DOT);

  lv_obj_t* ops = ui::c::card(col);
  cmd_btn(ops, "再生 / 一時停止", watch::MediaCmd::PlayPause);
  cmd_btn(ops, "次へ", watch::MediaCmd::Next);
  cmd_btn(ops, "前へ", watch::MediaCmd::Prev);
  cmd_btn(ops, "音量＋", watch::MediaCmd::VolUp);
  cmd_btn(ops, "音量－", watch::MediaCmd::VolDown);

  refresh();
  return scr;
}

void on_event(lv_obj_t*, const watch::Event& e) {
  if (e.type == watch::EventType::MediaStateChanged) refresh();
}

}  // namespace

namespace ui {
extern const ScreenOps kMediaScreen = {watch::Route::Media, build, on_event};
}
