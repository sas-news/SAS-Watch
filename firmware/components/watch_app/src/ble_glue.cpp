// ble_glue.cpp — ble_link (NimBLE タスク) ↔ core/UI の橋渡し。
//   - dispatch_req   : NimBLE タスクから呼ばれる。core_lock で app タスクと
//                      直列化してから core/protocol の dispatcher へ。
//   - on_conn_state  : pending に書いて app タスクで Event 化 (bus.publish は
//                      app タスク内から呼ぶ前提なのでここでは出さない)。
//   - on_passkey     : pending に書いて app タスクで ui::request_passkey。
//   - bulk_*         : 保存先はまだ無い (Phase 8 のテーマ/アセット/OTA)。
//                      NULL のまま = ble_link が検証のみ行う。
#include "internal.hpp"

#if CONFIG_SAS_BLE_LINK

#include <cstring>

#include "ble_link.h"
#include "board/board.hpp"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "ui/ui.hpp"
#include "watch/features/memo.hpp"
#include "watch/protocol/dispatch.hpp"

namespace watch_app {

namespace {

constexpr const char* TAG = "watch_app.ble";

// NimBLE タスク → app タスクへの保留値。
//   conn: -1 未設定 / 0 切断 / 1 接続 / 2 接続(認証済)
//   key : -1 無し / 0-999999 表示中パスキー
volatile int8_t s_pending_conn = -1;
volatile int32_t s_pending_key = -1;

// ---- dispatch_req -----------------------------------------------------------

size_t ble_dispatch(const uint8_t* req, size_t req_len, uint8_t* res,
                    size_t res_cap, void*) {
  core_lock();
  watch::proto::Services svc{};
  svc.clock = clock();
  svc.kv = kv();
  svc.settings = &settings_mut();
  svc.bus = &bus();
  svc.power = &power();
  svc.input = &input();
  svc.battery_percent = [](void*) { return board::pmic::battery_percent(); };
  svc.is_charging = [](void*) { return board::pmic::is_charging(); };
  svc.fw_version = [](void*) { return esp_app_get_description()->version; };
  svc.free_heap = [](void*) {
    return static_cast<int64_t>(esp_get_free_heap_size());
  };
  svc.free_psram = [](void*) {
    return static_cast<int64_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  };
  svc.timer_start = [](uint32_t seconds, void* c) {
    watch::Action a{};
    a.type = watch::ActionType::TimerStart;
    a.source = watch::ActionSource::Phone;
    a.arg0 = seconds;
    return features().handle(a, *static_cast<watch::FeatureContext*>(c));
  };
  svc.timer_stop = [](void* c) {
    watch::Action a{};
    a.type = watch::ActionType::TimerStop;
    a.source = watch::ActionSource::Phone;
    return features().handle(a, *static_cast<watch::FeatureContext*>(c));
  };
  svc.memo_create = [](const char* text, size_t len, void* c) {
    return watch::features::memo_create(
        text, len, *static_cast<watch::FeatureContext*>(c));
  };
  svc.ctx = fctx();

  watch::cbor::Writer w(res, res_cap);
  const watch::proto::DispatchError err =
      watch::proto::dispatch_req(req, req_len, svc, &w);
  core_unlock();
  if (err != watch::proto::DispatchError::Ok) {
    ESP_LOGD(TAG, "dispatch err=%d", static_cast<int>(err));
  }
  return w.size();
}

// ---- コールバック (NimBLE タスク) -------------------------------------------

void on_conn_state(bool connected, bool authenticated, void*) {
  s_pending_conn = connected ? (authenticated ? 2 : 1) : 0;
  wake_task();
}

void on_passkey(uint32_t passkey, void*) {
  s_pending_key = static_cast<int32_t>(passkey);
  wake_task();
}

ble_link_config_t s_cfg = {
    .dispatch_req = ble_dispatch,
    .dispatch_ctx = nullptr,
    .on_conn_state = on_conn_state,
    .on_passkey = on_passkey,
    .cb_ctx = nullptr,
    .bulk_begin = nullptr,
    .bulk_write = nullptr,
    .bulk_commit = nullptr,
    .bulk_abort = nullptr,
    .bulk_ctx = nullptr,
};

}  // namespace

const ble_link_config_t* ble_config() { return &s_cfg; }

// app タスクのループで保留分を処理する。
void ble_glue_poll() {
  const int8_t conn = s_pending_conn;
  if (conn >= 0) {
    s_pending_conn = -1;
    power().set_ble_connected(conn > 0);
    bus().publish({watch::EventType::BleConnChanged,
                   static_cast<uint32_t>(conn > 0 ? 1 : 0)});
    ESP_LOGI(TAG, "ble %s%s", conn > 0 ? "connected" : "disconnected",
             conn == 2 ? " (authenticated)" : "");
  }
  const int32_t key = s_pending_key;
  if (key >= 0) {
    s_pending_key = -1;
    ui::request_passkey(static_cast<uint32_t>(key));
  }
}

}  // namespace watch_app

#else  // !CONFIG_SAS_BLE_LINK

namespace watch_app {
void ble_glue_poll() {}
}  // namespace watch_app

#endif
