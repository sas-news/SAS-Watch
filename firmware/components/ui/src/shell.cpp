// shell.cpp — 画面管理・Event 中継・ジェスチャー・タイマー終了アラート。
//   app タスク (EventBus) → 固定リング → lv_async_call → LVGL タスクで再描画。
//   LVGL を触るのはここ (LVGL タスク) と screens/* (on_event は LVGL タスク)。
#include "screens/screens.hpp"

#include "components.hpp"
#include "ui/port.hpp"
#include "ui/ui.hpp"
#include "watch/platform.hpp"

#include <cstdio>

namespace ui {

namespace {

constexpr uint8_t kRingCap = 16;

Ctx s_ctx;
ActionSink s_sink = nullptr;

lv_obj_t* s_home = nullptr;         // Home は消さずに使い回す
lv_obj_t* s_cur = nullptr;          // 現在の画面ルート
const ScreenOps* s_ops = nullptr;
uint8_t s_last_depth = 1;
lv_obj_t* s_alert = nullptr;
lv_timer_t* s_alert_timer = nullptr;

// BLE パスキー確認
lv_obj_t* s_key_modal = nullptr;
lv_timer_t* s_key_timer = nullptr;
volatile int32_t s_pending_key = -1;
void (*s_passkey_confirm)(bool accept) = nullptr;

watch::Event s_ring[kRingCap];
volatile uint8_t s_rh = 0;
volatile uint8_t s_rt = 0;

// ---- Action 出口 ----------------------------------------------------------

void emit_nav(watch::Route r) {
  emit(watch::ActionType::Navigate, static_cast<uint32_t>(r));
}

// ---- Event → LVGL タスク中継 ----------------------------------------------

void drain_events(void*);

void bus_cb(const watch::Event& e, void*) {
  port::crit_enter();
  const uint8_t nx = static_cast<uint8_t>((s_rh + 1) % kRingCap);
  if (nx != s_rt) {
    s_ring[s_rh] = e;
    s_rh = nx;
  }
  port::crit_exit();
  // lv_async_call は lv_timer リストを触るので lock 下で登録する。
  if (port::lock(50)) {
    lv_async_call(drain_events, nullptr);
    port::unlock();
  }
}

void handle_event(const watch::Event& e);

void drain_events(void*) {
  for (;;) {
    port::crit_enter();
    if (s_rt == s_rh) {
      port::crit_exit();
      break;
    }
    const watch::Event e = s_ring[s_rt];
    s_rt = static_cast<uint8_t>((s_rt + 1) % kRingCap);
    port::crit_exit();
    handle_event(e);
  }
}

// ---- 画面遷移 --------------------------------------------------------------

void del_old_screen(lv_timer_t* t) {
  lv_obj_t* old = static_cast<lv_obj_t*>(lv_timer_get_user_data(t));
  if (old && old != s_home && old != s_cur) lv_obj_delete(old);
}

void on_gesture(lv_event_t*) {
  const lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
  const watch::Route cur = s_ctx.nav ? s_ctx.nav->current() : watch::Route::Home;
  switch (d) {
    case LV_DIR_RIGHT:
      emit(watch::ActionType::Back);
      break;
    case LV_DIR_BOTTOM:
      // 下スワイプ = クイック設定 (Home のみ)
      if (cur == watch::Route::Home) emit_nav(watch::Route::Quick);
      break;
    case LV_DIR_TOP:
      // 上スワイプ = アプリ一覧 (Home のみ)
      if (cur == watch::Route::Home) emit_nav(watch::Route::More);
      break;
    case LV_DIR_LEFT:
      break;  // 予約 (将来タイル)
    default:
      break;
  }
}

void on_long_press(lv_event_t*) {
  // 長押し = 電源メニュー (PWR 長押しと同じ動作)
  emit_nav(watch::Route::PowerMenu);
}

void attach_input(lv_obj_t* scr) {
  lv_obj_add_flag(
      scr, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE |
                                      LV_OBJ_FLAG_EVENT_BUBBLE));
  lv_obj_add_event_cb(scr, on_gesture, LV_EVENT_GESTURE, nullptr);
  lv_obj_add_event_cb(scr, on_long_press, LV_EVENT_LONG_PRESSED, nullptr);
}

void swap_screen(watch::Route r, bool rebuild = false) {
  const ScreenOps* ops = screen_ops(r);
  if (!ops) {
    // 画面未実装の Route (Notifications/Media/Dev/Agent/Confirm) は無視。
    watch::log_write(watch::LogLevel::Warn, "ui",
                     "no screen for route; staying");
    return;
  }
  const uint8_t depth = s_ctx.nav ? s_ctx.nav->depth() : 1;
  const bool push = depth > s_last_depth;
  s_last_depth = depth;
  const lv_screen_load_anim_t anim =
      push ? LV_SCREEN_LOAD_ANIM_MOVE_LEFT : LV_SCREEN_LOAD_ANIM_MOVE_RIGHT;
  const Theme& t = theme();

  if (r == watch::Route::Home && s_home && !rebuild) {
    lv_obj_t* prev = lv_screen_active();
    lv_screen_load_anim(s_home, anim, t.anim_ms, 0, false);
    s_cur = s_home;
    s_ops = ops;
    if (prev && prev != s_home) {
      lv_timer_t* tm = lv_timer_create(del_old_screen, t.anim_ms + 60, prev);
      lv_timer_set_repeat_count(tm, 1);
    }
    if (s_ops->on_event) {
      // 再表示時に時刻・状態を最新化するためダミーの Tick を流す。
      s_ops->on_event(s_home, {watch::EventType::ClockTick, 0});
    }
    return;
  }

  lv_obj_t* prev = lv_screen_active();
  lv_obj_t* scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr, t.bg, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  if (r == watch::Route::Home) s_home = scr;
  attach_input(scr);
  ops->create(scr);
  lv_screen_load_anim(scr, anim, t.anim_ms, 0, false);
  s_cur = scr;
  s_ops = ops;
  if (prev && prev != s_home) {
    lv_timer_t* tm = lv_timer_create(del_old_screen, t.anim_ms + 60, prev);
    lv_timer_set_repeat_count(tm, 1);
  }
}

// ---- タイマー終了アラート ---------------------------------------------------

void hide_alert(lv_timer_t*) {
  if (s_alert) {
    lv_obj_delete(s_alert);
    s_alert = nullptr;
  }
  s_alert_timer = nullptr;
}

void show_timer_alert() {
  if (s_alert) return;  // 既出なら重複させない
  const Theme& t = theme();
  s_alert = lv_obj_create(lv_layer_top());
  lv_obj_remove_flag(s_alert, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(s_alert, 410, 502);
  lv_obj_set_pos(s_alert, 0, 0);
  lv_obj_set_style_bg_color(s_alert, lv_color_hex(0x2A1000), 0);
  lv_obj_set_style_bg_opa(s_alert, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s_alert, 0, 0);
  lv_obj_add_flag(s_alert, LV_OBJ_FLAG_CLICKABLE);

  // テーマ画像スロット: タイマー終了時の画像 (あれば文字の上)。
  if (t.img_timer_done) {
    lv_obj_t* img = lv_image_create(s_alert);
    lv_image_set_src(img, t.img_timer_done);
    lv_obj_align(img, LV_ALIGN_CENTER, 0, -160);
    lv_obj_add_flag(img, LV_OBJ_FLAG_EVENT_BUBBLE);
  }

  lv_obj_t* l = lv_label_create(s_alert);
  lv_label_set_text(l, "タイマー終了！");
  lv_obj_set_style_text_font(l, t.font_title, 0);
  lv_obj_set_style_text_color(l, t.accent, 0);
  lv_obj_align(l, LV_ALIGN_CENTER, 0, -60);

  lv_obj_t* b = lv_button_create(s_alert);
  lv_obj_set_size(b, 240, 64);
  lv_obj_align(b, LV_ALIGN_CENTER, 0, 60);
  lv_obj_set_style_radius(b, t.radius_lg, 0);
  lv_obj_set_style_bg_color(b, t.primary, 0);
  lv_obj_t* bl = lv_label_create(b);
  lv_label_set_text(bl, "止める");
  lv_obj_set_style_text_font(bl, t.font_title, 0);
  lv_obj_set_style_text_color(bl, t.on_primary, 0);
  lv_obj_center(bl);
  lv_obj_add_event_cb(
      b,
      [](lv_event_t*) {
        emit(watch::ActionType::TimerReset);
        hide_alert(nullptr);
      },
      LV_EVENT_CLICKED, nullptr);

  s_alert_timer = lv_timer_create(hide_alert, 15000, nullptr);
  lv_timer_set_repeat_count(s_alert_timer, 1);
  port::vibrate(800);  // TODO(hw): 実機で確認 — 振動強さ/パターン
}

// ---- パスキー確認モーダル ---------------------------------------------------

void hide_passkey(lv_timer_t*) {
  if (s_key_modal) {
    lv_obj_delete(s_key_modal);
    s_key_modal = nullptr;
  }
  s_key_timer = nullptr;
}

void show_passkey(uint32_t passkey) {
  if (s_key_modal) lv_obj_delete(s_key_modal);
  const Theme& t = theme();
  s_key_modal = lv_obj_create(lv_layer_top());
  lv_obj_remove_flag(s_key_modal, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(s_key_modal, 410, 502);
  lv_obj_set_pos(s_key_modal, 0, 0);
  lv_obj_set_style_bg_color(s_key_modal, lv_color_hex(0x0A1420), 0);
  lv_obj_set_style_bg_opa(s_key_modal, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s_key_modal, 0, 0);
  lv_obj_add_flag(s_key_modal, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t* title = lv_label_create(s_key_modal);
  lv_label_set_text(title, "ペアリング確認");
  lv_obj_set_style_text_font(title, t.font_title, 0);
  lv_obj_set_style_text_color(title, t.text, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 48);

  lv_obj_t* key = lv_label_create(s_key_modal);
  lv_obj_set_style_text_font(key, t.font_digits, 0);
  lv_obj_set_style_text_color(key, t.accent, 0);
  char kb[8];
  std::snprintf(kb, sizeof(kb), "%06lu",
                static_cast<unsigned long>(passkey % 1000000));
  lv_label_set_text(key, kb);
  lv_obj_align(key, LV_ALIGN_CENTER, 0, -40);

  lv_obj_t* q = lv_label_create(s_key_modal);
  lv_label_set_text(q, "スマホの数字と同じ？");
  lv_obj_set_style_text_font(q, t.font_body, 0);
  lv_obj_set_style_text_color(q, t.text_dim, 0);
  lv_obj_align(q, LV_ALIGN_CENTER, 0, 40);

  lv_obj_t* yes = lv_button_create(s_key_modal);
  lv_obj_set_size(yes, 300, 64);
  lv_obj_align(yes, LV_ALIGN_CENTER, 0, 120);
  lv_obj_set_style_radius(yes, t.radius_lg, 0);
  lv_obj_set_style_bg_color(yes, t.primary, 0);
  lv_obj_t* yl = lv_label_create(yes);
  lv_label_set_text(yl, "はい");
  lv_obj_set_style_text_font(yl, t.font_title, 0);
  lv_obj_set_style_text_color(yl, t.on_primary, 0);
  lv_obj_center(yl);
  lv_obj_add_event_cb(
      yes,
      [](lv_event_t*) {
        if (s_passkey_confirm) s_passkey_confirm(true);
        hide_passkey(nullptr);
      },
      LV_EVENT_CLICKED, nullptr);

  lv_obj_t* no = lv_button_create(s_key_modal);
  lv_obj_set_size(no, 300, 64);
  lv_obj_align(no, LV_ALIGN_CENTER, 0, 196);
  lv_obj_set_style_radius(no, t.radius_lg, 0);
  lv_obj_set_style_bg_color(no, t.surface2, 0);
  lv_obj_t* nl = lv_label_create(no);
  lv_label_set_text(nl, "いいえ");
  lv_obj_set_style_text_font(nl, t.font_title, 0);
  lv_obj_set_style_text_color(nl, t.text, 0);
  lv_obj_center(nl);
  lv_obj_add_event_cb(
      no,
      [](lv_event_t*) {
        if (s_passkey_confirm) s_passkey_confirm(false);
        hide_passkey(nullptr);
      },
      LV_EVENT_CLICKED, nullptr);

  // 30秒で自動で閉じる (リンク側のペアリングタイムアウトで拒否される)。
  s_key_timer = lv_timer_create(hide_passkey, 30000, nullptr);
  lv_timer_set_repeat_count(s_key_timer, 1);
}

void drain_passkey(void*) {
  const int32_t k = s_pending_key;
  if (k < 0) return;
  s_pending_key = -1;
  show_passkey(static_cast<uint32_t>(k));
}

// テーマ切替で現在画面を作り直す (Home は作り直して差し替える)。
void rebuild_current() {
  const watch::Route r =
      s_ctx.nav ? s_ctx.nav->current() : watch::Route::Home;
  if (r == watch::Route::Home) {
    s_home = nullptr;  // 旧 Home は swap_screen の prev として遅延削除される
    swap_screen(r, true);
  } else {
    swap_screen(r, true);  // prev はアニメ後に削除される
  }
}

void handle_event(const watch::Event& e) {
  if (e.type == watch::EventType::RouteChanged) {
    const watch::Route r =
        s_ctx.nav ? s_ctx.nav->current() : watch::Route::Home;
    if (!s_cur || (s_ops && s_ops->route != r)) swap_screen(r);
  } else if (e.type == watch::EventType::TimerFinished) {
    show_timer_alert();
  } else if (e.type == watch::EventType::ThemeChanged) {
    // settings.theme を適用 (失敗時は適用層が standard に倒す)。
    const char* id = s_ctx.settings ? s_ctx.settings->theme : "standard";
    theme_apply(id);
    rebuild_current();
    // アラート表示中なら画像差し替わりに合わせて閉じる。
    if (s_alert) hide_alert(nullptr);
  }
  if (s_ops && s_ops->on_event && s_cur) s_ops->on_event(s_cur, e);
}

}  // namespace

// ---- 公開 API ---------------------------------------------------------------

const Ctx& ctx() { return s_ctx; }

void set_passkey_confirm(void (*fn)(bool accept)) {
  s_passkey_confirm = fn;
}

// どのタスクからでも: 保留値に書いて LVGL タスクへ async で投げる。
void request_passkey(uint32_t passkey) {
  s_pending_key = static_cast<int32_t>(passkey);
  if (port::lock(50)) {
    lv_async_call(drain_passkey, nullptr);
    port::unlock();
  }
}

void set_action_sink(ActionSink sink) { s_sink = sink; }

void emit(watch::ActionType type, uint32_t arg0) {
  if (!s_sink) return;
  watch::Action a{};
  a.type = type;
  a.source = watch::ActionSource::Touch;
  a.arg0 = arg0;
  s_sink(a);
}

void emit_text(watch::ActionType type, const char* text) {
  if (!s_sink || !text) return;
  watch::Action a{};
  a.type = type;
  a.source = watch::ActionSource::Touch;
  a.set_text(text);
  s_sink(a);
}

bool create(const Ctx& c) {
  s_ctx = c;
  if (s_ctx.bus) {
    s_ctx.bus->subscribe(watch::EventType::RouteChanged, bus_cb, nullptr);
    s_ctx.bus->subscribe(watch::EventType::ClockTick, bus_cb, nullptr);
    s_ctx.bus->subscribe(watch::EventType::BatteryChanged, bus_cb, nullptr);
    s_ctx.bus->subscribe(watch::EventType::ChargingChanged, bus_cb, nullptr);
    s_ctx.bus->subscribe(watch::EventType::BleConnChanged, bus_cb, nullptr);
    s_ctx.bus->subscribe(watch::EventType::TimerStarted, bus_cb, nullptr);
    s_ctx.bus->subscribe(watch::EventType::TimerStopped, bus_cb, nullptr);
    s_ctx.bus->subscribe(watch::EventType::TimerFinished, bus_cb, nullptr);
    s_ctx.bus->subscribe(watch::EventType::StopwatchChanged, bus_cb, nullptr);
    s_ctx.bus->subscribe(watch::EventType::CounterChanged, bus_cb, nullptr);
    s_ctx.bus->subscribe(watch::EventType::MemoSaved, bus_cb, nullptr);
    s_ctx.bus->subscribe(watch::EventType::MemoDeleted, bus_cb, nullptr);
    s_ctx.bus->subscribe(watch::EventType::BrightnessChanged, bus_cb, nullptr);
    s_ctx.bus->subscribe(watch::EventType::SettingsChanged, bus_cb, nullptr);
    s_ctx.bus->subscribe(watch::EventType::PowerStateChanged, bus_cb, nullptr);
    s_ctx.bus->subscribe(watch::EventType::ThemeChanged, bus_cb, nullptr);
    s_ctx.bus->subscribe(watch::EventType::AgentStatusChanged, bus_cb,
                         nullptr);
  }
  // 設定されたテーマを最初の画面構築より先に適用する。
  if (s_ctx.settings) theme_apply(s_ctx.settings->theme);
  // 最初のアクティブ画面は破棄して Home に置き換える。
  lv_obj_t* initial = lv_screen_active();
  swap_screen(watch::Route::Home);
  if (initial && initial != s_cur) {
    lv_timer_t* tm = lv_timer_create(del_old_screen, theme().anim_ms + 60,
                                     initial);
    lv_timer_set_repeat_count(tm, 1);
  }
  return s_cur != nullptr;
}

}  // namespace ui
