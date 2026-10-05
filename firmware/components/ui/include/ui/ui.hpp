// ui.hpp — 時計 UI の公開 API。
//   Theme (Token) → Component → Screen の3段。Feature は LVGL を知らない。
//   core の Event を EventBus 購読で受け、LVGL タスクに投げ直して再描画する。
//   表示 410x502、ダークテーマ、全コントロール日本語テキストラベル、
//   指で押せる最小 48px。
#pragma once

#include <cstdint>

#include "lvgl.h"
#include "watch/action.hpp"
#include "watch/event.hpp"
#include "watch/event_bus.hpp"
#include "watch/feature.hpp"
#include "watch/navigation.hpp"
#include "watch/settings.hpp"

namespace ui {

// 画面が必要とする core への窓口。
struct Ctx {
  watch::EventBus* bus = nullptr;
  watch::Navigator* nav = nullptr;
  const watch::Settings* settings = nullptr;
  // FeatureContext (audio の有無や録音経過を知りたい画面用)。無くても動く。
  watch::FeatureContext* fctx = nullptr;
};

// Action の出口。watch_app (firmware) / sim が Runtime のキューへつなぐ。
using ActionSink = void (*)(const watch::Action& a);
void set_action_sink(ActionSink sink);

// 画面・ジェスチャーからの Action を投げる。LVGL タスク内から呼ぶこと。
void emit(watch::ActionType type, uint32_t arg0 = 0);

// text を持つ Action (SetTheme など) を投げる。
void emit_text(watch::ActionType type, const char* text);

// firmware 用: パネル・LVGLタスク・タッチを立ち上げ、既定 display を返す。
// sim では自分で lv_display_create するので呼ばない。
lv_display_t* init_display();

// 既定 display のアクティブ画面に Home を組み立てる。
// firmware: lvgl_port_lock 保持中に呼ぶ。sim: 直接呼ぶ。
bool create(const Ctx& ctx);

// 画面が ctx を引けるように (shell.cpp が保持)。
const Ctx& ctx();

// BLE パスキー確認 (Numeric Comparison)。
// request_passkey はどのタスクからでも呼べる (内部で LVGL タスクへ中継)。
void request_passkey(uint32_t passkey);
// 「はい/いいえ」の出口。firmware は ble_link_confirm_passkey をつなぐ。
void set_passkey_confirm(void (*fn)(bool accept));

}  // namespace ui
