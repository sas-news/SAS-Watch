// agent.cpp — AI: 「話しかける」(音声入力→スマホ中継で文字起こし/LLM) と
// 定型質問ボタン。録音・保存はしない (音声は BLE で流すだけで、時計に
// ファイル/永続バッファは残らない)。文字起こし結果と Muse へのテキストだけ。
// 状態機械は core の features::agent_* が持ち、ここは表示と入力だけ。
// ui::c の共通部品と ui::theme() のトークンだけで組む (デザイン刷新と衝突しない)。
#include "components.hpp"
#include "theme.hpp"
#include "screens/screens.hpp"
#include "ui/ui.hpp"
#include "watch/features/agent.hpp"

#include <cstdio>

namespace {

struct M {
  lv_obj_t* talk_card = nullptr;
  lv_obj_t* talk_btn = nullptr;
  lv_obj_t* status_l = nullptr;
  lv_obj_t* notice_l = nullptr;     // 聞き取り中の警告 (旧アプリ検出など)
  lv_obj_t* rec_bar = nullptr;
  lv_obj_t* partial_cap = nullptr;  // 部分文字起こしの見出し
  lv_obj_t* partial_card = nullptr;
  lv_obj_t* partial_l = nullptr;
  lv_obj_t* ask_card = nullptr;
  lv_obj_t* ask_cap = nullptr;
  lv_obj_t* reply_card = nullptr;
  lv_obj_t* reply_l = nullptr;
  lv_obj_t* close_btn = nullptr;
};
M s;

watch::FeatureContext* fctx() { return ui::ctx().fctx; }

void set_btn_text(lv_obj_t* btn, const char* text) {
  lv_obj_t* l = lv_obj_get_child(btn, 0);
  if (l) lv_label_set_text(l, text);
}

void refresh() {
  using watch::features::AgentPhase;
  const AgentPhase p = watch::features::agent_phase();

  // 「話しかける」ボタン: Idle/Reply/Error で開始、Recording で停止送信。
  // Sending/Thinking 中は重複送信できないので隠す。
  switch (p) {
    case AgentPhase::Recording:
      set_btn_text(s.talk_btn, "停止して送信");
      lv_obj_remove_flag(s.talk_btn, LV_OBJ_FLAG_HIDDEN);
      break;
    case AgentPhase::Sending:
    case AgentPhase::Thinking:
      lv_obj_add_flag(s.talk_btn, LV_OBJ_FLAG_HIDDEN);
      break;
    default:
      set_btn_text(s.talk_btn, "話しかける");
      lv_obj_remove_flag(s.talk_btn, LV_OBJ_FLAG_HIDDEN);
      break;
  }

  // 状態行 + 入力レベルバー。
  char buf[48];
  const char* st = "";
  bool show_bar = false;
  switch (p) {
    case AgentPhase::Idle:
      // 音声入力であることを明示する (録音・保存はしない)。
      st = "マイクに向かって話してください\n（音声は保存されません）";
      break;
    case AgentPhase::Recording:
      if (fctx()) {
        std::snprintf(buf, sizeof(buf), "聞き取り中 %lu 秒 (最大 %lu)",
                      static_cast<unsigned long>(
                          watch::features::agent_record_elapsed_s(*fctx())),
                      static_cast<unsigned long>(
                          watch::features::kAgentMaxRecordSec));
        st = buf;
      } else {
        st = "聞き取り中";
      }
      show_bar = true;
      break;
    case AgentPhase::Sending: st = "送信中…"; break;
    case AgentPhase::Thinking: st = "考え中…"; break;
    case AgentPhase::Reply: st = "返答"; break;
    case AgentPhase::Error: st = "エラー"; break;
  }
  lv_label_set_text(s.status_l, st);

  // 聞き取り中の警告 (スマホが dictation 非対応など)。
  const char* notice =
      (p == AgentPhase::Recording) ? watch::features::agent_notice_text() : "";
  if (notice && *notice) {
    lv_label_set_text(s.notice_l, notice);
    lv_obj_remove_flag(s.notice_l, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s.notice_l, LV_OBJ_FLAG_HIDDEN);
  }

  if (show_bar && fctx()) {
    lv_obj_remove_flag(s.rec_bar, LV_OBJ_FLAG_HIDDEN);
    lv_bar_set_value(s.rec_bar,
                     watch::features::agent_record_level(*fctx()),
                     LV_ANIM_OFF);
  } else {
    lv_obj_add_flag(s.rec_bar, LV_OBJ_FLAG_HIDDEN);
  }

  // 部分文字起こし (聞き取り中は大きく表示。停止後も Thinking 中は確定版を残す)。
  const char* partial = watch::features::agent_partial_text();
  const bool partial_final = watch::features::agent_partial_final();
  const bool dictating = watch::features::agent_dictating();
  const bool show_partial =
      p == AgentPhase::Recording || (p == AgentPhase::Sending && dictating) ||
      (p == AgentPhase::Thinking && partial && *partial);
  if (show_partial) {
    const bool confirmed = partial_final || p == AgentPhase::Thinking;
    lv_label_set_text(s.partial_cap, confirmed ? "聞き取り" : "認識中");
    lv_label_set_text(s.partial_l,
                      (partial && *partial) ? partial : "話してください…");
    lv_obj_remove_flag(s.partial_cap, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s.partial_card, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s.partial_cap, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s.partial_card, LV_OBJ_FLAG_HIDDEN);
  }

  // 送信中/考え中/返答/エラーカード (「話しかける」のすぐ下、定型質問より上)。
  const char* reply_txt = nullptr;
  bool show_close = false;
  switch (p) {
    case AgentPhase::Sending:
      reply_txt = dictating ? "音声を送信しています…" : "送信中…";
      break;
    case AgentPhase::Thinking:
      // 確定テキストがあるときは部分文字起こしカードに出しているので、
      // 重複する「考え中…」カードは出さない (状態行には出る)。
      if (!(partial && *partial)) reply_txt = "考え中…";
      break;
    case AgentPhase::Reply:
      reply_txt = watch::features::agent_reply_text();
      show_close = true;
      break;
    case AgentPhase::Error:
      reply_txt = watch::features::agent_error_text();
      show_close = true;
      break;
    default: break;
  }
  if (reply_txt) {
    lv_label_set_text(s.reply_l, reply_txt);
    lv_obj_remove_flag(s.reply_card, LV_OBJ_FLAG_HIDDEN);
    if (show_close) {
      lv_obj_remove_flag(s.close_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(s.close_btn, LV_OBJ_FLAG_HIDDEN);
    }
  } else {
    lv_obj_add_flag(s.reply_card, LV_OBJ_FLAG_HIDDEN);
  }

  // 定型質問ボタンは空欄が全てのときカードごと隠す。
  const watch::Settings* stg = fctx() ? fctx()->settings : nullptr;
  bool any = false;
  for (size_t i = 0; i < watch::features::kAgentQuestionCount; ++i) {
    if (watch::features::agent_question(i, stg)) any = true;
  }
  if (any) {
    lv_obj_remove_flag(s.ask_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s.ask_cap, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s.ask_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s.ask_cap, LV_OBJ_FLAG_HIDDEN);
  }
}

lv_obj_t* build(lv_obj_t* scr) {
  const ui::Theme& t = ui::theme();
  lv_obj_set_style_bg_color(scr, t.bg, 0);
  ui::c::header(scr, "AI", true);
  lv_obj_t* col = ui::c::content(scr);
  s = M{};

  // 「話しかける」: 音声入力トグル (PrimaryAction と同じ)。音声は保存しない。
  s.talk_card = ui::c::card(col);
  s.talk_btn = ui::c::button_primary(s.talk_card, "話しかける",
      [](lv_event_t*) {
        ui::emit(watch::ActionType::AgentRecordToggle);
      },
      nullptr);
  s.status_l = ui::c::line(s.talk_card, "");
  s.notice_l = ui::c::line(s.talk_card, "");
  lv_obj_set_style_text_color(s.notice_l, t.danger, 0);
  lv_obj_add_flag(s.notice_l, LV_OBJ_FLAG_HIDDEN);
  s.rec_bar = lv_bar_create(s.talk_card);
  lv_bar_set_range(s.rec_bar, 0, 100);
  lv_obj_set_size(s.rec_bar, LV_PCT(100), 14);
  lv_obj_set_style_bg_color(s.rec_bar, t.surface2, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s.rec_bar, t.primary, LV_PART_INDICATOR);

  // 部分文字起こしカード: 聞き取り中は本文より大きいフォント (26px) で
  // 折り返し表示する。長文は content 全体のスクロールで読む。
  // 表示するのは文字起こしのテキストだけで、音声のバッファ/ファイルは持たない。
  s.partial_cap = ui::c::caption(col, "認識中");
  s.partial_card = ui::c::card(col);
  lv_obj_set_flex_align(s.partial_card, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  s.partial_l = lv_label_create(s.partial_card);
  lv_obj_set_width(s.partial_l, LV_PCT(100));
  lv_obj_set_style_text_font(s.partial_l, t.font_row, 0);
  lv_obj_set_style_text_color(s.partial_l, t.text, 0);
  lv_label_set_long_mode(s.partial_l, LV_LABEL_LONG_WRAP);
  lv_obj_add_flag(s.partial_cap, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s.partial_card, LV_OBJ_FLAG_HIDDEN);

  // 返答/エラー表示カード (「話しかける」のすぐ下、定型質問より上。
  // 長文はラベル折り返し + content 全体スクロール)。
  s.reply_card = ui::c::card(col);
  lv_obj_set_flex_align(s.reply_card, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  s.reply_l = lv_label_create(s.reply_card);
  lv_obj_set_width(s.reply_l, LV_PCT(100));
  lv_obj_set_style_text_font(s.reply_l, t.font_body, 0);
  lv_obj_set_style_text_color(s.reply_l, t.text, 0);
  lv_label_set_long_mode(s.reply_l, LV_LABEL_LONG_WRAP);
  s.close_btn = ui::c::button(s.reply_card, "閉じる",
      [](lv_event_t*) { ui::emit(watch::ActionType::AgentClear); },
      nullptr);
  lv_obj_add_flag(s.reply_card, LV_OBJ_FLAG_HIDDEN);

  // 定型質問ボタン (settings agent.q1..3 の非空分)。
  s.ask_cap = ui::c::caption(col, "定型質問");
  s.ask_card = ui::c::group(col);
  const watch::Settings* stg = fctx() ? fctx()->settings : nullptr;
  for (size_t i = 0; i < watch::features::kAgentQuestionCount; ++i) {
    const char* q = watch::features::agent_question(i, stg);
    if (!q) continue;
    ui::c::row(s.ask_card, q, nullptr, nullptr, true,
        [](lv_event_t* e) {
          const auto i = static_cast<uint32_t>(
              reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
          ui::emit(watch::ActionType::AgentAsk, i);
        },
        reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
  }

  refresh();
  return scr;
}

void on_event(lv_obj_t*, const watch::Event& e) {
  // 聞き取り秒数は ClockTick、状態/質問の変化はそれぞれの Event で再描画。
  if (e.type == watch::EventType::ClockTick ||
      e.type == watch::EventType::AgentStatusChanged ||
      e.type == watch::EventType::AgentPartialChanged ||
      e.type == watch::EventType::SettingsChanged) {
    refresh();
  }
}

}  // namespace

namespace ui {
extern const ScreenOps kAgentScreen = {watch::Route::Agent, build, on_event};
}
