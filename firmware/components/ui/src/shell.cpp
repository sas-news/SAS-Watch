// shell.cpp — 画面管理・Event 中継・ジェスチャー・タイマー終了アラート。
//   app タスク (EventBus) → 固定リング → lv_async_call → LVGL タスクで再描画。
//   LVGL を触るのはここ (LVGL タスク) と screens/* (on_event は LVGL タスク)。
#include "screens/screens.hpp"

#include "components.hpp"
#include "faces/faces.hpp"
#include "perf.hpp"
#include "theme_res.hpp"
#include "ui/port.hpp"
#include "ui/ui.hpp"
#include "watch/features/alarm.hpp"
#include "watch/features/notify.hpp"
#include "watch/platform.hpp"

// disp->scr_to_load / prev_scr を掃くために private 構造体が必要
#include "lvgl_private.h"

LV_FONT_DECLARE(font_num_72);

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
lv_timer_t* s_alarm_beep_timer = nullptr;  // アラーム繰り返しビープ
lv_obj_t* s_alert_flash = nullptr;         // T9: 点滅する全面レイヤ
lv_timer_t* s_flash_timer = nullptr;       // T9: 500ms 点滅
lv_timer_t* s_vibe_timer = nullptr;        // T9: 2s ごとの振動
uint32_t s_flash_count = 0;
#if SAS_APP_NOTIFY
lv_obj_t* s_toast = nullptr;               // 通知ポップアップ
lv_timer_t* s_toast_timer = nullptr;
#endif

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

// アニメ後の旧画面削除タイマー (最大 4 件管理しておき、
// テーマ切替で即破棄するとき対象が重複しないよう引き算する)。
lv_timer_t* s_del_timers[4] = {};

void del_old_screen(lv_timer_t* t) {
  for (auto& s : s_del_timers) {
    if (s == t) {
      s = nullptr;
      break;
    }
  }
  lv_obj_t* old = static_cast<lv_obj_t*>(lv_timer_get_user_data(t));
  if (old && old != s_home && old != s_cur) {
    lv_obj_delete(old);
  }
}

void schedule_delete(lv_obj_t* old, uint32_t ms) {
  // 同じ画面の削除タイマーが既にあれば二重登録しない
  // (連続遷移で同じ prev が2回予約されると lv_obj_delete が
  //  二重に走ってヒープを壊す)。
  for (auto& s : s_del_timers) {
    if (s && lv_timer_get_user_data(s) == old) return;
  }
  lv_timer_t* tm = lv_timer_create(del_old_screen, ms, old);
  lv_timer_set_repeat_count(tm, 1);
  for (auto& s : s_del_timers) {
    if (!s) {
      s = tm;
      return;
    }
  }
}

// テーマ切替用: 旧テーマ資源 (binfont/画像 dsc) を参照する全画面を
// 同期破棄する。遷移アニメで旧画面が解放済みフォントを描くと落ちるので
// ThemeChanged はアニメを捨てる (apply 中は描画されない)。
void drop_screens_now() {
  for (auto& s : s_del_timers) {
    if (!s) continue;
    lv_obj_t* o = static_cast<lv_obj_t*>(lv_timer_get_user_data(s));
    if (o == s_cur || o == s_home) {
      lv_timer_delete(s);
      s = nullptr;
    }
  }
  if (s_home && s_home != s_cur) lv_obj_delete(s_home);
  s_home = nullptr;
  if (s_cur) lv_obj_delete(s_cur);  // act_scr は NULL に戻る
  s_cur = nullptr;
  s_ops = nullptr;
  // LVGL は削除された画面の参照を act_scr しか消さない。
  // 遷移アニメ中だと scr_to_load / prev_scr が解放済み画面を指したまま
  // 残り、次の lv_screen_load_anim が use-after-free を踏むので明示的に掃く。
  lv_display_t* d = lv_display_get_default();
  if (d) {
    d->scr_to_load = nullptr;
    d->prev_scr = nullptr;
    d->act_scr = nullptr;
  }
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

// ---- テーマ v2: 画面スキン (bg + scrim) --------------------------------

// Route → kThemeScreenNames の添字 (-1 = スキン対象外)。
// 順序は navigation.hpp の Route enum の宣言順。
constexpr int8_t kRouteScreen[] = {
    /*None*/ -1,
    /*Home*/ 0,           // home
    /*Quick*/ 1,          // quick
    /*Notifications*/ 12, // notifications
    /*Timer*/ 3,          // timer
    /*Stopwatch*/ 4,      // stopwatch
    /*Counter*/ 5,        // counter
    /*Steps*/ 6,          // steps
    /*Memo*/ 7,           // memo
    /*More*/ 2,           // more
    /*Settings*/ 8,       // settings
    /*About*/ -1,
    /*Media*/ 13,         // media
    /*Dev*/ -1,
    /*Agent*/ 14,         // agent
    /*PowerMenu*/ 10,     // powermenu
    /*Ota*/ 9,            // ota
    /*Confirm*/ -1,
    /*Alarm*/ 11,         // alarm
};
static_assert(sizeof(kRouteScreen) == 19, "Route enum と同期");

// 背景画像が消えるとき画面ブロックを返す。
void scr_skin_free(lv_event_t* e) {
  port::theme_screen_free(static_cast<uint8_t*>(lv_event_get_user_data(e)));
}

// ---- スキン画像ブロックの小アロケータ (T8: bg + deco で共有) ----
// 先頭 512B を dsc 領域 (最大8枚)、画像データは +512 から連続配置。
struct SkinAlloc {
  uint8_t* blk = nullptr;
  size_t dsc_off = 0;
  size_t data_off = 0;
};
SkinAlloc s_skin;
constexpr size_t kSkinDscArea = 512;

lv_obj_t* skin_img(lv_obj_t* scr, const char* name, int x, int y, bool front) {
  if (!s_skin.blk ||
      s_skin.dsc_off + sizeof(lv_image_dsc_t) > kSkinDscArea) {
    return nullptr;
  }
  lv_image_dsc_t* dsc = reinterpret_cast<lv_image_dsc_t*>(
      s_skin.blk + s_skin.dsc_off);
  const uint32_t cap = ui::port::kThemeScreenBlockSize -
                       kSkinDscArea - static_cast<uint32_t>(s_skin.data_off);
  const uint32_t wrote = theme_res::img_decode(
      ui::theme_id(), name, s_skin.blk + kSkinDscArea + s_skin.data_off,
      cap, 410, 502, dsc);
  if (!wrote) return nullptr;
  s_skin.dsc_off += sizeof(lv_image_dsc_t);
  s_skin.data_off += wrote;
  lv_obj_t* im = lv_image_create(scr);
  lv_image_set_src(im, dsc);
  // 負値 = 右端/下端基準 (画像サイズを引く)。
  const int px = x < 0 ? 410 + x - static_cast<int>(dsc->header.w) : x;
  const int py = y < 0 ? 502 + y - static_cast<int>(dsc->header.h) : y;
  lv_obj_set_pos(im, px, py);
  lv_obj_add_flag(im, LV_OBJ_FLAG_EVENT_BUBBLE);  // タップは透過
  (void)front;
  return im;
}

// スキン (画面別 > "*") の bg+scrim+deco(back) を scr に敷く。
// 画像は screen ブロック (640KiB×2) にデコード; scr の DELETE で返す。
void skin_base(lv_obj_t* scr, int8_t sidx) {
  const watch::ThemeScreenSkin* sk =
      sidx >= 0 ? ui::theme_screen_skin(sidx) : nullptr;
  s_skin = SkinAlloc{};
  if (!sk) return;
  const bool need_blk = sk->bg[0] || sk->deco_count > 0;
  if (need_blk) s_skin.blk = port::theme_screen_block();
  int z = 0;
  if (sk->bg[0] && s_skin.blk) {
    lv_obj_t* im = skin_img(scr, sk->bg, 0, 0, false);
    if (im) lv_obj_move_to_index(im, z++);
  }
  if (sk->scrim) {
    // 黒の薄膜で背景だけを暗くする (この後の画面部品は上に載る)。
    lv_obj_t* sc = lv_obj_create(scr);
    lv_obj_set_size(sc, 410, 502);
    lv_obj_set_pos(sc, 0, 0);
    lv_obj_set_style_bg_color(sc, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(sc, sk->scrim, 0);
    lv_obj_set_style_border_width(sc, 0, 0);
    lv_obj_remove_flag(sc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sc, LV_OBJ_FLAG_EVENT_BUBBLE);  // ジェスチャーは素通り
    lv_obj_move_to_index(sc, z++);
  }
  // deco z=back → 内容の背面 (scrim の上)。
  for (int i = 0; i < sk->deco_count; ++i) {
    const auto& d = sk->deco[i];
    if (d.front) continue;
    lv_obj_t* im = skin_img(scr, d.img, d.x, d.y, false);
    if (im) lv_obj_move_to_index(im, z++);
  }
  // ブロックが空っぽなら返す (全部 decode 失敗)。
  if (s_skin.blk && s_skin.data_off == 0) {
    port::theme_screen_free(s_skin.blk);
    s_skin.blk = nullptr;
  } else if (s_skin.blk) {
    lv_obj_add_event_cb(scr, scr_skin_free, LV_EVENT_DELETE, s_skin.blk);
  }
}

// deco z=front → 画面の全内容より上 (生成順 = 最後)。タップは透過。
void skin_front(lv_obj_t* scr, int8_t sidx) {
  const watch::ThemeScreenSkin* sk =
      sidx >= 0 ? ui::theme_screen_skin(sidx) : nullptr;
  if (!sk) return;
  for (int i = 0; i < sk->deco_count; ++i) {
    const auto& d = sk->deco[i];
    if (!d.front) continue;
    skin_img(scr, d.img, d.x, d.y, true);
  }
}

// Route → ログ用の画面名 (テーマ名表 + スキン対象外の3種)。
const char* route_name(watch::Route r) {
  const int8_t sidx = kRouteScreen[static_cast<int>(r)];
  if (sidx >= 0) return watch::kThemeScreenNames[sidx];
  switch (r) {
    case watch::Route::About: return "about";
    case watch::Route::Dev: return "dev";
    case watch::Route::Confirm: return "confirm";
    default: return "none";
  }
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
    perf_mark_screen(route_name(r));
    if (prev && prev != s_home) schedule_delete(prev, t.anim_ms + 60);
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
  const int8_t sidx = kRouteScreen[static_cast<int>(r)];
  ui::theme_apply_screen_tokens(sidx);  // v4: 画面単位の色上書き
  skin_base(scr, sidx);  // 画面スキン (bg+scrim+deco back) は最背面
  if (r == watch::Route::Home) s_home = scr;
  attach_input(scr);
  ops->create(scr);
  c::mascot(scr, sidx);  // 画面隅マスコット (有効画面のみ)
  skin_front(scr, sidx);  // deco z=front は全内容の上
  // 旧画面が無い状態 (テーマ切替の同期破棄直後) はアニメ無しで即ロード。
  if (prev) {
    lv_screen_load_anim(scr, anim, t.anim_ms, 0, false);
  } else {
    lv_screen_load_anim(scr, LV_SCREEN_LOAD_ANIM_NONE, 0, 0, false);
  }
  s_cur = scr;
  s_ops = ops;
  perf_mark_screen(route_name(r));
  if (prev && prev != s_home) schedule_delete(prev, t.anim_ms + 60);
}

// ---- タイマー終了アラート ---------------------------------------------------

// ---- T9: 画面フラッシュ通知 (音の代わり) -------------------------------------
// 鳴動中はアラート背面の全面レイヤを 500ms 周期で白/黒に切り替え、
// 輝度を 100% にする。60 秒 or 何か入力で点滅停止 (アラート自体は残る)。

void flash_stop() {
  if (s_flash_timer) {
    lv_timer_delete(s_flash_timer);
    s_flash_timer = nullptr;
  }
  if (s_vibe_timer) {
    lv_timer_delete(s_vibe_timer);
    s_vibe_timer = nullptr;
  }
  if (s_alert_flash) {
    // 残りが黒のまま消えないよう透明に戻してから消す。
    lv_obj_set_style_bg_opa(s_alert_flash, LV_OPA_TRANSP, 0);
    s_alert_flash = nullptr;
  }
  port::brightness_apply(
      static_cast<int>(s_ctx.settings ? s_ctx.settings->brightness : 50));
}

void flash_tick(lv_timer_t*) {
  if (!s_alert_flash) {
    flash_stop();
    return;
  }
  ++s_flash_count;
  const bool white = (s_flash_count & 1u) != 0;
  lv_obj_set_style_bg_color(
      s_alert_flash,
      white ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x000000), 0);
  if (s_flash_count >= 120) flash_stop();  // 60s で点滅終了
}

void flash_start(lv_obj_t* alert) {
  s_flash_count = 0;
  // alert の最初の子として作り、内容の背面に置く。
  s_alert_flash = lv_obj_create(alert);
  lv_obj_remove_flag(s_alert_flash, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(s_alert_flash, 410, 502);
  lv_obj_set_pos(s_alert_flash, 0, 0);
  lv_obj_set_style_bg_opa(s_alert_flash, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(s_alert_flash, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(s_alert_flash, 0, 0);
  port::brightness_apply(100);
  s_flash_timer = lv_timer_create(flash_tick, 500, nullptr);
  s_vibe_timer = lv_timer_create(
      [](lv_timer_t*) { port::vibrate(600); }, 2000, nullptr);
}

void hide_alert(lv_timer_t*) {
  flash_stop();
  if (s_alert) {
    lv_obj_delete(s_alert);
    s_alert = nullptr;
  }
  s_alert_timer = nullptr;
}

// フル画面アラート共通のピルボタン (横幅 366 = モックの 22px 余白)。
lv_obj_t* alert_button(lv_obj_t* parent, const char* text, bool primary,
                       lv_event_cb_t cb) {
  const Theme& t = theme();
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_size(b, 346, 58);  // 410 - 2·32 (コンテンツ幅)
  lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(b, 0, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  if (primary) {
    lv_obj_set_style_bg_color(b, t.primary, 0);
  } else {
    lv_obj_set_style_bg_color(b, t.surface2, 0);
  }
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, t.font_body, 0);
  lv_obj_set_style_text_color(l, primary ? t.on_primary : t.text, 0);
  lv_obj_center(l);
  // 画像スキンがあればボタン part で描く (無ければ上のベクタのまま)。
  ui::c::skin_obj(b, primary ? watch::kSkinBtnPrimary
                             : watch::kSkinBtnSecondary);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  return b;
}

void show_timer_alert() {
  if (s_alert) return;  // 既出なら重複させない
  const Theme& t = theme();
  s_alert = lv_obj_create(lv_layer_top());
  lv_obj_remove_flag(s_alert, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(s_alert, 410, 502);
  lv_obj_set_pos(s_alert, 0, 0);
  // テーマの obj pad (30) が効くと align の下端計算が中身領域基準になるので 0。
  lv_obj_set_style_pad_all(s_alert, 0, 0);
  lv_obj_set_style_bg_color(s_alert, t.bg, 0);
  lv_obj_set_style_bg_opa(s_alert, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s_alert, 0, 0);
  lv_obj_add_flag(s_alert, LV_OBJ_FLAG_CLICKABLE);
  // T9: 背面に点滅レイヤを敷き、入力があれば点滅だけ止める。
  flash_start(s_alert);
  lv_obj_add_event_cb(
      s_alert, [](lv_event_t*) { flash_stop(); }, LV_EVENT_CLICKED,
      nullptr);

  // テーマスキン: screens["alert"] があればそちらを敷く。
  // 無ければ暖色グロー (ラジアルグラデーション。静的描画)。
  const watch::ThemeScreenSkin* ask =
      ui::theme_screen_skin(watch::theme_screen_index("alert"));
  if (ask && (ask->bg[0] || ask->scrim)) {
    skin_base(s_alert, watch::theme_screen_index("alert"));
  } else {
    // 描画負荷対策 (T5): radial grad は全面補間が重いので単色 + 細いリング。
    lv_obj_set_style_bg_color(s_alert, lv_color_hex(0x140B05), 0);
    lv_obj_t* ring = lv_obj_create(s_alert);
    lv_obj_set_size(ring, 140, 140);  // 00:00 の背面だけ (「タイマー終了」を跨がない)
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, -34);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(0x5A2A0A), 0);
    lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ring, LV_OBJ_FLAG_EVENT_BUBBLE);
  }

  // テーマ画像スロット: タイマー終了時の画像 (あれば文字の上)。
  lv_obj_t* hero = nullptr;
  if (t.img_timer_done) {
    hero = lv_image_create(s_alert);
    lv_image_set_src(hero, t.img_timer_done);
    lv_obj_align(hero, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_add_flag(hero, LV_OBJ_FLAG_EVENT_BUBBLE);
  }

  // "TIMER" 見出し (primary2, 字送り広め)。画像があるときは重なるので省略。
  if (!hero) {
    lv_obj_t* tag = lv_label_create(s_alert);
    lv_label_set_text(tag, "TIMER");
    lv_obj_set_style_text_font(tag, t.font_body, 0);
    lv_obj_set_style_text_color(tag, t.primary2, 0);
    lv_obj_set_style_text_letter_space(tag, 8, 0);
    lv_obj_align(tag, LV_ALIGN_CENTER, 0, -108);
  }

  // 00:00 は等幅数字フォント (JetBrains Mono 72px)。
  lv_obj_t* digits = lv_label_create(s_alert);
  lv_label_set_text(digits, "00:00");
  lv_obj_set_style_text_font(digits, &font_num_72, 0);
  lv_obj_set_style_text_color(digits, t.text, 0);
  lv_obj_align(digits, LV_ALIGN_CENTER, 0, -34);

  lv_obj_t* l = lv_label_create(s_alert);
  lv_label_set_text(l, "タイマー終了");
  lv_obj_set_style_text_font(l, t.font_title, 0);
  lv_obj_set_style_text_color(l, t.text, 0);
  lv_obj_align(l, LV_ALIGN_CENTER, 0, 60);  // 00:00 との間に +12px

  // 止める (primary) / もう1分 (secondary: +60秒して再開)
  lv_obj_t* stop = alert_button(
      s_alert, "止める", true,
      [](lv_event_t*) {
        emit(watch::ActionType::TimerReset);
        hide_alert(nullptr);
      });
  lv_obj_align(stop, LV_ALIGN_BOTTOM_MID, 0, -100);
  lv_obj_t* more = alert_button(
      s_alert, "もう1分", false,
      [](lv_event_t*) {
        emit(watch::ActionType::TimerAddMinute);
        hide_alert(nullptr);
      });
  lv_obj_align(more, LV_ALIGN_BOTTOM_MID, 0, -28);

  s_alert_timer = lv_timer_create(hide_alert, 15000, nullptr);
  lv_timer_set_repeat_count(s_alert_timer, 1);
  port::vibrate(800);  // TODO(hw): 実機で確認 — 振動強さ/パターン
}

// ---- アラーム鳴動アラート ---------------------------------------------------
// 「止める」「スヌーズ」縦並び。鳴動中は ~4.5s ごとにビープ+短振動を繰り返す。

void hide_alarm_alert() {
  flash_stop();
  if (s_alert) {
    lv_obj_delete(s_alert);
    s_alert = nullptr;
  }
  if (s_alarm_beep_timer) {
    lv_timer_delete(s_alarm_beep_timer);
    s_alarm_beep_timer = nullptr;
  }
  s_alert_timer = nullptr;
}

void alarm_beep(lv_timer_t*) {
  if (s_ctx.fctx && s_ctx.fctx->audio) {
    const uint8_t vol = static_cast<uint8_t>(
        s_ctx.settings && s_ctx.settings->audio_volume > 100
            ? 100
            : (s_ctx.settings ? s_ctx.settings->audio_volume : 70));
    s_ctx.fctx->audio->beep(watch::BeepKind::Alarm, vol);
  }
  port::vibrate(300);  // TODO(hw): 実機で確認 — 繰り返し振動の強さ/間隔
}

void show_alarm_alert(uint32_t id) {
  hide_alert(nullptr);   // タイマーアラートが出ていれば消す
  if (s_alert) hide_alarm_alert();  // 連続発火時は作り直し
  const Theme& t = theme();
  s_alert = lv_obj_create(lv_layer_top());
  lv_obj_remove_flag(s_alert, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(s_alert, 410, 502);
  lv_obj_set_pos(s_alert, 0, 0);
  lv_obj_set_style_pad_all(s_alert, 0, 0);  // align を画面座標どおりにする
  lv_obj_set_style_bg_color(s_alert, t.bg, 0);
  lv_obj_set_style_bg_opa(s_alert, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s_alert, 0, 0);
  lv_obj_add_flag(s_alert, LV_OBJ_FLAG_CLICKABLE);
  // テーマスキン (screens["alert"]) があれば背景に敷く。
  skin_base(s_alert, watch::theme_screen_index("alert"));

  // T9: 背面に点滅レイヤ、入力で点滅停止。
  flash_start(s_alert);
  lv_obj_add_event_cb(
      s_alert, [](lv_event_t*) { flash_stop(); }, LV_EVENT_CLICKED,
      nullptr);

  lv_obj_t* l = lv_label_create(s_alert);
  lv_label_set_text(l, "アラーム");
  lv_obj_set_style_text_font(l, t.font_title, 0);
  lv_obj_set_style_text_color(l, t.accent, 0);
  lv_obj_align(l, LV_ALIGN_CENTER, 0, -170);

  // 鳴っているアラームの時刻 (id が見つからなければ時刻は出さない)。
  // 時刻は等幅数字フォント (JetBrains Mono 72px)。
#if SAS_APP_ALARM
  watch::features::AlarmEntry e;
  if (watch::features::alarm_find(id, &e)) {
    char tb[8];
    std::snprintf(tb, sizeof(tb), "%u:%02u", e.hour, e.min);
    lv_obj_t* tm = lv_label_create(s_alert);
    lv_label_set_text(tm, tb);
    lv_obj_set_style_text_font(tm, &font_num_72, 0);
    lv_obj_set_style_text_color(tm, t.text, 0);
    lv_obj_align(tm, LV_ALIGN_CENTER, 0, -70);
  }
#else
  (void)id;
#endif

  lv_obj_t* stop = alert_button(
      s_alert, "止める", true,
      [](lv_event_t*) {
        emit(watch::ActionType::AlarmStop);
        hide_alarm_alert();
      });
  lv_obj_align(stop, LV_ALIGN_BOTTOM_MID, 0, -100);

  lv_obj_t* snz = alert_button(
      s_alert, "スヌーズ (5分)", false,
      [](lv_event_t*) {
        emit(watch::ActionType::AlarmSnooze);
        hide_alarm_alert();
      });
  lv_obj_align(snz, LV_ALIGN_BOTTOM_MID, 0, -28);

  // 鳴動中は繰り返す (停止/スヌーズ/タイムアウトでアラートが消えると止まる)。
  s_alarm_beep_timer = lv_timer_create(alarm_beep, 4500, nullptr);
  alarm_beep(nullptr);  // 初回は即鳴らす
  port::vibrate(600);   // TODO(hw): 実機で確認
}

// ---- 通知ポップアップ -------------------------------------------------------
// 画面上部に短時間だけ出すトースト。タップ操作は下に通す。

#if SAS_APP_NOTIFY
void hide_toast(lv_timer_t*) {
  if (s_toast) {
    lv_obj_delete(s_toast);
    s_toast = nullptr;
  }
  s_toast_timer = nullptr;
}
#endif

void show_notify_popup() {
#if SAS_APP_NOTIFY
  watch::features::NotifyEntry e;
  if (!watch::features::notify_at(0, &e)) return;
  hide_toast(nullptr);
  const Theme& t = theme();
  s_toast = lv_obj_create(lv_layer_top());
  lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_SCROLLABLE);
  // 上端の丸角を避けるためコンテンツ余白 (x=32, y=36) 内に置く。
  lv_obj_set_size(s_toast, 346, 96);
  lv_obj_set_pos(s_toast, 32, 36);
  lv_obj_set_style_bg_color(s_toast, t.surface2, 0);
  lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s_toast, 0, 0);
  lv_obj_set_style_radius(s_toast, t.radius_lg, 0);
  lv_obj_set_flex_flow(s_toast, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_toast, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                       LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(s_toast, 12, 0);
  lv_obj_set_style_pad_row(s_toast, 4, 0);

  lv_obj_t* app = lv_label_create(s_toast);
  lv_label_set_text(app, e.app);
  lv_obj_set_style_text_font(app, t.font_body, 0);
  lv_obj_set_style_text_color(app, t.text_dim, 0);

  lv_obj_t* title = lv_label_create(s_toast);
  lv_label_set_text(title, e.title);
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  lv_obj_set_width(title, LV_PCT(100));
  lv_obj_set_style_text_font(title, t.font_body, 0);
  lv_obj_set_style_text_color(title, t.text, 0);

  s_toast_timer = lv_timer_create(hide_toast, 4000, nullptr);
  lv_timer_set_repeat_count(s_toast_timer, 1);
  if (!s_ctx.settings || s_ctx.settings->notify_vibrate) {
    port::vibrate(150);  // TODO(hw): 実機で確認 — 通知の振動強さ
  }
#endif  // SAS_APP_NOTIFY
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
  lv_obj_set_style_pad_all(s_key_modal, 0, 0);  // align を画面座標どおりにする
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
  // 96px だと6桁がコンテンツ幅をはみ出すので 72px の等幅数字にする。
  lv_obj_set_style_text_font(key, &font_num_72, 0);
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

  lv_obj_t* yes = alert_button(
      s_key_modal, "はい", true,
      [](lv_event_t*) {
        if (s_passkey_confirm) s_passkey_confirm(true);
        hide_passkey(nullptr);
      });
  lv_obj_align(yes, LV_ALIGN_BOTTOM_MID, 0, -100);

  lv_obj_t* no = alert_button(
      s_key_modal, "いいえ", false,
      [](lv_event_t*) {
        if (s_passkey_confirm) s_passkey_confirm(false);
        hide_passkey(nullptr);
      });
  lv_obj_align(no, LV_ALIGN_BOTTOM_MID, 0, -28);

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
  } else if (e.type == watch::EventType::AlarmRinging) {
    show_alarm_alert(e.arg0);
#if SAS_APP_ALARM
  } else if (e.type == watch::EventType::AlarmChanged) {
    // 停止/スヌーズ/自動停止で鳴動が終わったら閉じる。
    if (!watch::features::alarm_ringing() && s_alert) hide_alarm_alert();
#endif
  } else if (e.type == watch::EventType::NotificationPosted) {
    show_notify_popup();
  } else if (e.type == watch::EventType::ThemeChanged) {
    // settings.theme を適用 (失敗時は適用層が standard に倒す)。
    // 旧テーマ資源 (binfont/画像) は旧画面が参照したまま → 先に全画面を
    // 同期破棄してから適用・再構築 (遷移アニメ中の描画で落ちるのを防ぐ)。
    const char* id = s_ctx.settings ? s_ctx.settings->theme : "standard";
    drop_screens_now();
    theme_apply(id);
    rebuild_current();
    // アラート表示中なら画像差し替わりに合わせて閉じる。
    if (s_alert) hide_alert(nullptr);
  } else if (e.type == watch::EventType::FaceChanged) {
    // settings.face / clock_font を反映するため現在の画面を組み直す
    // (Home なら文字盤が差し替わり、設定画面なら「使用中」が更新される)。
    rebuild_current();
  }
  if (s_ops && s_ops->on_event && s_cur) s_ops->on_event(s_cur, e);
}

}  // namespace

// ---- 公開 API ---------------------------------------------------------------

const Ctx& ctx() { return s_ctx; }

const char* current_screen_name() {
  return route_name(s_ctx.nav ? s_ctx.nav->current() : watch::Route::Home);
}

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
    s_ctx.bus->subscribe(watch::EventType::TimerPaused, bus_cb, nullptr);
    s_ctx.bus->subscribe(watch::EventType::StopwatchChanged, bus_cb, nullptr);
    s_ctx.bus->subscribe(watch::EventType::CounterChanged, bus_cb, nullptr);
    s_ctx.bus->subscribe(watch::EventType::StepsChanged, bus_cb, nullptr);
    s_ctx.bus->subscribe(watch::EventType::MemoSaved, bus_cb, nullptr);
    s_ctx.bus->subscribe(watch::EventType::MemoDeleted, bus_cb, nullptr);
    s_ctx.bus->subscribe(watch::EventType::BrightnessChanged, bus_cb, nullptr);
    s_ctx.bus->subscribe(watch::EventType::SettingsChanged, bus_cb, nullptr);
    s_ctx.bus->subscribe(watch::EventType::PowerStateChanged, bus_cb, nullptr);
    s_ctx.bus->subscribe(watch::EventType::ThemeChanged, bus_cb, nullptr);
    s_ctx.bus->subscribe(watch::EventType::AlarmRinging, bus_cb, nullptr);
    s_ctx.bus->subscribe(watch::EventType::AlarmChanged, bus_cb, nullptr);
    s_ctx.bus->subscribe(watch::EventType::NotificationPosted, bus_cb,
                         nullptr);
    s_ctx.bus->subscribe(watch::EventType::NotificationsCleared, bus_cb,
                         nullptr);
    s_ctx.bus->subscribe(watch::EventType::MediaStateChanged, bus_cb, nullptr);
    s_ctx.bus->subscribe(watch::EventType::OtaProgress, bus_cb, nullptr);
    s_ctx.bus->subscribe(watch::EventType::FaceChanged, bus_cb, nullptr);
    s_ctx.bus->subscribe(watch::EventType::AgentStatusChanged, bus_cb,
                         nullptr);
  }
  // 設定されたテーマを最初の画面構築より先に適用する。
  if (s_ctx.settings) theme_apply(s_ctx.settings->theme);
  // 最初のアクティブ画面は破棄して Home に置き換える。
  lv_obj_t* initial = lv_screen_active();
  swap_screen(watch::Route::Home);
  if (initial && initial != s_cur) {
    schedule_delete(initial, theme().anim_ms + 60);
  }
  return s_cur != nullptr;
}

}  // namespace ui
