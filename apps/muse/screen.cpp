// screen.cpp — Muse 画面 (Phase 2 ⑤ UI 字幕)。
//
// 状態カード (未設定/接続中/待機/送信中/エラー) と、Muse の返答本文を
// ページ表示する。返答は core の features::muse_* が 960B まで保持し、
// 300B 目安 (UTF-8 境界) のページに分けて 15 秒ごとに自動で次へ送る。
// 画面タップまたは「次へ」で手動送り (最後まで行ったら先頭へ戻る)。
//
// 秘密 (ホスト/token) は表示しない。返答本文はログに出さない。
// LVGL はこの画面の build/on_event/コールバック (LVGL タスク) の中だけで触る。
#include "components.hpp"
#include "screens/screens.hpp"
#include "theme.hpp"
#include "ui/ui.hpp"

#include <cstdio>

#include "watch/features/muse.hpp"

namespace {

using watch::MuseState;

struct M {
  lv_obj_t* state_cap = nullptr;
  lv_obj_t* state_l = nullptr;
  lv_obj_t* error_l = nullptr;
  lv_obj_t* toggle_btn = nullptr;
  lv_obj_t* reply_cap = nullptr;
  lv_obj_t* reply_card = nullptr;
  lv_obj_t* reply_l = nullptr;
  lv_obj_t* page_l = nullptr;
  lv_obj_t* next_btn = nullptr;
};
M s;

void set_text(lv_obj_t* l, const char* text) {
  if (l) lv_label_set_text(l, text ? text : "");
}

const char* state_text(bool available, MuseState st) {
  if (!available) return "このファームでは Muse が無効です";
  switch (st) {
    case MuseState::Unconfigured:
      return "Muse の設定がありません\n"
             "（サーバーに接続して資格情報を投入してください）";
    case MuseState::Off:
      return "停止中（「接続」で Muse につなぎます）";
    case MuseState::Starting:
      return "接続中…（Wi-Fi / セッション起動待ち）";
    case MuseState::Ready:
      return "接続済み・待機中";
    case MuseState::Busy:
      return "送信中／返答待ち…";
    case MuseState::Error:
      return "エラー";
    case MuseState::Unavailable:
    default:
      return "このファームでは Muse が無効です";
  }
}

// 接続/切断ボタンのラベル。押せない状態は nullptr (非表示)。
const char* toggle_label(bool available, MuseState st) {
  if (!available) return nullptr;
  switch (st) {
    case MuseState::Off:
    case MuseState::Error:
      return "接続";
    case MuseState::Starting:
    case MuseState::Ready:
    case MuseState::Busy:
      return "切断";
    case MuseState::Unconfigured:
    case MuseState::Unavailable:
    default:
      return nullptr;
  }
}

void refresh() {
  const bool avail = watch::features::muse_available();
  const MuseState st = watch::features::muse_state();

  set_text(s.state_l, state_text(avail, st));

  // エラー詳細 (日本語)。firmware の last_error をそのまま出す。
  const char* err = watch::features::muse_error();
  if (avail && st == MuseState::Error && err && *err) {
    set_text(s.error_l, err);
    lv_obj_remove_flag(s.error_l, LV_OBJ_FLAG_HIDDEN);
  } else {
    set_text(s.error_l, "");
    lv_obj_add_flag(s.error_l, LV_OBJ_FLAG_HIDDEN);
  }

  // 接続/切断 (PrimaryAction)。
  const char* label = toggle_label(avail, st);
  if (label) {
    set_text(lv_obj_get_child(s.toggle_btn, 0), label);
    lv_obj_remove_flag(s.toggle_btn, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s.toggle_btn, LV_OBJ_FLAG_HIDDEN);
  }

  // 返答本文 (1 ページ分) とページ番号。
  const uint16_t pages = watch::features::muse_pages();
  char page_text[watch::features::kMusePageBytes + 8];
  bool has_page = false;
  if (avail && pages > 0) {
    has_page = watch::features::muse_page_text(page_text, sizeof(page_text));
  }
  if (has_page) {
    set_text(s.reply_l, page_text);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%u/%u ページ（15 秒で次へ）",
                  static_cast<unsigned>(watch::features::muse_page() + 1),
                  static_cast<unsigned>(pages));
    set_text(s.page_l, buf);
    lv_obj_remove_flag(s.page_l, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s.reply_cap, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s.reply_card, LV_OBJ_FLAG_HIDDEN);
    if (pages > 1) {
      lv_obj_remove_flag(s.next_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(s.next_btn, LV_OBJ_FLAG_HIDDEN);
    }
  } else {
    // 返答がまだ無いときも、接続状態に応じた案内を出す。
    const char* hint = "まだ返答はありません";
    if (!avail) {
      hint = "Muse を有効にしたファームで使えます";
    } else if (st == MuseState::Unconfigured) {
      hint = "設定を投入すると、ここに返答が表示されます";
    } else if (st == MuseState::Off) {
      hint = "「接続」を押すと Muse につながります";
    }
    set_text(s.reply_l, hint);
    set_text(s.page_l, "");
    lv_obj_add_flag(s.page_l, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s.reply_cap, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s.reply_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s.next_btn, LV_OBJ_FLAG_HIDDEN);
  }
}

// 「次へ」ボタン。ボタンの CLICKED はカードへバブルするので、カード側は
// target がカード自身のときだけ進める (二重送り防止)。
void next_page_and_refresh(lv_event_t*) {
  watch::features::muse_next_page();
  refresh();
}

void reply_card_clicked(lv_event_t* e) {
  if (lv_event_get_target_obj(e) != s.reply_card) return;
  next_page_and_refresh(e);
}

lv_obj_t* build(lv_obj_t* scr) {
  const ui::Theme& t = ui::theme();
  lv_obj_set_style_bg_color(scr, t.bg, 0);
  ui::c::header(scr, "Muse", true);
  lv_obj_t* col = ui::c::content(scr);
  s = M{};

  // 1) 状態カード: 状態 (大きめ) + エラー詳細。
  lv_obj_t* st_card = ui::c::card(col);
  s.state_cap = ui::c::caption(st_card, "状態");
  s.state_l = lv_label_create(st_card);
  lv_obj_set_width(s.state_l, LV_PCT(100));
  lv_obj_set_style_text_font(s.state_l, t.font_row, 0);
  lv_obj_set_style_text_color(s.state_l, t.text, 0);
  lv_label_set_long_mode(s.state_l, LV_LABEL_LONG_WRAP);
  s.error_l = lv_label_create(st_card);
  lv_obj_set_width(s.error_l, LV_PCT(100));
  lv_obj_set_style_text_font(s.error_l, t.font_body, 0);
  lv_obj_set_style_text_color(s.error_l, t.danger, 0);
  lv_label_set_long_mode(s.error_l, LV_LABEL_LONG_WRAP);
  lv_obj_add_flag(s.error_l, LV_OBJ_FLAG_HIDDEN);
  // 接続/切断 (画面の主アクション)。Unavailable/Unconfigured では refresh が隠す。
  s.toggle_btn = ui::c::button_primary(
      st_card, "接続",
      [](lv_event_t*) { ui::emit(watch::ActionType::PrimaryAction); }, nullptr);

  // 2) 返答カード: 本文 (折り返し) + ページ表示 + 次へ。
  //    カードのタップでも次ページへ (15 秒の自動送りを待たずに読める)。
  s.reply_cap = ui::c::caption(col, "返答");
  s.reply_card = ui::c::card(col);
  lv_obj_add_flag(s.reply_card, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s.reply_card, reply_card_clicked, LV_EVENT_CLICKED,
                      nullptr);
  s.reply_l = lv_label_create(s.reply_card);
  lv_obj_set_width(s.reply_l, LV_PCT(100));
  lv_obj_set_style_text_font(s.reply_l, t.font_body, 0);
  lv_obj_set_style_text_color(s.reply_l, t.text, 0);
  lv_label_set_long_mode(s.reply_l, LV_LABEL_LONG_WRAP);
  s.page_l = lv_label_create(s.reply_card);
  lv_obj_set_width(s.page_l, LV_PCT(100));
  lv_obj_set_style_text_font(s.page_l, t.font_body, 0);
  lv_obj_set_style_text_color(s.page_l, t.text_dim, 0);
  lv_label_set_long_mode(s.page_l, LV_LABEL_LONG_WRAP);
  s.next_btn = ui::c::button(s.reply_card, "次へ", next_page_and_refresh,
                             nullptr);
  lv_obj_add_flag(s.next_btn, LV_OBJ_FLAG_HIDDEN);

  refresh();
  return scr;
}

void on_event(lv_obj_t*, const watch::Event& e) {
  // 1 秒ごとの ClockTick で状態変化とページ送りを反映する
  // (feature が画面表示中だけ 1 秒周期で app タスクを起こす)。
  if (e.type == watch::EventType::ClockTick ||
      e.type == watch::EventType::SettingsChanged) {
    refresh();
  }
}

}  // namespace

namespace ui {
extern const ScreenOps kMuseScreen = {watch::Route::App1, build, on_event};
}
