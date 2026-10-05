// watch_app — core::Runtime を回すアプリ層。
//   app タスク: ActionQueue → Runtime::step → PowerApply → 電源遷移。
//   入力: board ボタン → InputMapper、LVGL ジェスチャー → emit (ui)。
//   BLE: ble_link の配線 (dispatch/接続通知/パスキー) も持つ (ble_glue)。
#pragma once

#include "sdkconfig.h"
#if CONFIG_SAS_BLE_LINK
#include "ble_link.h"
#endif

#include "watch/event_bus.hpp"
#include "watch/navigation.hpp"
#include "watch/platform.hpp"
#include "watch/runtime.hpp"
#include "watch/settings.hpp"

namespace watch_app {

struct Deps {
  watch::Clock* clock = nullptr;
  watch::KeyValueStore* kv = nullptr;
};

// 起動。UI (ui::create) は別途 LVGL ロック内で呼ぶこと。
bool start(const Deps& deps);

// どのタスク/コールバックからでも呼べる Action 投入。
// queue はロックフックでスレッド安全。app タスクへ notify する。
void push_action(const watch::Action& a);

watch::Runtime& runtime();
watch::Navigator& navigator();
watch::EventBus& bus();
const watch::Settings& settings();

#if CONFIG_SAS_BLE_LINK
// app_main が ble_link_start() に渡す設定。
// dispatch_req / on_conn_state / on_passkey / bulk_* を配線済み。
const ble_link_config_t* ble_config();
#endif

}  // namespace watch_app
