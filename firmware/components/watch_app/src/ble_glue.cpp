// ble_glue.cpp — ble_link (NimBLE タスク) ↔ core/UI の橋渡し。
//   - dispatch_req   : NimBLE タスクから呼ばれる。core_lock で app タスクと
//                      直列化してから core/protocol の dispatcher へ。
//   - on_conn_state  : pending に書いて app タスクで Event 化 (bus.publish は
//                      app タスク内から呼ぶ前提なのでここでは出さない)。
//   - on_passkey     : pending に書いて app タスクで ui::request_passkey。
//   - bulk_out       : 音声メモの時計→スマホ送信 (app タスクでポンプ)。
//   - on_bus_evt     : EventBus の Event を EVT として notify する。
//   - bulk_*         : 受信 theme_store が littlefs に受けて展開・適用待ち登録。
#include "internal.hpp"

#if CONFIG_SAS_BLE_LINK

#include <cstring>

#include "ble_link.h"
#include "board/board.hpp"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "ota/ota.hpp"
#include "theme_store/theme_store.hpp"
#include "ui/ui.hpp"
#include "wifi/wifi.hpp"
#include "watch/features/agent.hpp"
#include "watch/features/memo.hpp"
#include "watch/features/steps.hpp"
#include "watch/protocol/bulk.hpp"
#include "watch/protocol/dispatch.hpp"
#include "watch/protocol/frame.hpp"
#include "watch/protocol/sha256.hpp"

namespace watch_app {

namespace {

constexpr const char* TAG = "watch_app.ble";

// NimBLE タスク → app タスクへの保留値。
//   conn: -1 未設定 / 0 切断 / 1 接続 / 2 接続(認証済)
//   key : -1 無し / 0-999999 表示中パスキー
volatile int8_t s_pending_conn = -1;
volatile int32_t s_pending_key = -1;

// ---- bulk_out (時計→スマホの push 送信) ------------------------------------
// Phone は受信側として BULK_ACK{next} を返す (8チャンクごと/END後)。
// ACK が来ない/進まない部分はタイムアウトで acked 位置から送り直す。
// kind="memo" (音声メモ) と "agent_audio" (AI の録音) で共用。1本ずつしか流せない。
struct BulkOut {
  bool active = false;
  uint16_t id = 0;        // BULK の transfer id
  uint32_t file_id = 0;   // AudioPort 上の音声ファイル id
  void (*done)(uint16_t id, bool ok) = nullptr;  // app タスクから呼ぶ
  uint32_t size = 0;
  uint32_t sent = 0;      // 送信したバイト位置
  uint32_t acked = 0;     // Phone が受理したバイト位置
  uint8_t inflight = 0;   // ACK 待ちのチャンク数
  uint8_t retries = 0;
  int64_t last_ms = 0;
  bool end_sent = false;
};
constexpr size_t kBulkOutChunk = 512;
constexpr uint8_t kBulkOutWindow = 8;           // kBulkAckEvery と同じ
constexpr uint8_t kBulkOutMaxRetry = 5;
constexpr int64_t kBulkOutAckTimeoutMs = 1200;
BulkOut s_out;

void bulk_out_stop(const char* why) {
  ESP_LOGW(TAG, "bulk out id=%u stopped: %s", s_out.id, why);
  s_out.active = false;
  if (s_out.done) s_out.done(s_out.id, false);
}

// file_id の音声ファイルを BULK kind で送る。tid は transfer id (返答の照合に使う)。
// 完了/中断は done (nullptr 可) に app タスクで知らせる。
bool bulk_out_begin(uint32_t file_id, const char* kind, uint16_t tid,
                    void (*done)(uint16_t, bool)) {
  if (s_out.active || !ble_link_bulk_ready() || !fctx() || !fctx()->audio) {
    return false;
  }
  uint32_t size = 0;
  if (!fctx()->audio->memo_audio_size(file_id, &size) || size == 0) {
    return false;
  }
  // ファイル全体の sha256 を流しながら計算する。
  watch::proto::Sha256 h;
  uint8_t buf[256];
  uint32_t off = 0;
  for (;;) {
    size_t len = sizeof(buf);
    if (!fctx()->audio->memo_audio_read(file_id, off, buf, &len)) return false;
    if (len == 0) break;
    h.update(buf, len);
    off += len;
  }
  if (off != size) return false;
  uint8_t sha[32];
  h.finish(sha);

  uint8_t start[96];
  const size_t n = watch::proto::bulk_encode_start(
      tid, kind, size, sha, kBulkOutChunk, start, sizeof(start));
  if (n == 0) return false;
  if (ble_link_bulk_send(0x10, tid, start, n) != ESP_OK) return false;
  s_out = BulkOut{};
  s_out.active = true;
  s_out.id = tid;
  s_out.file_id = file_id;
  s_out.done = done;
  s_out.size = size;
  s_out.last_ms = clock()->now_ms();
  ESP_LOGI(TAG, "bulk out start kind=%s file=%lu size=%lu", kind,
           static_cast<unsigned long>(file_id),
           static_cast<unsigned long>(size));
  return true;
}

// NimBLE タスクから届いた Phone 側の ACK {id,next}。
void on_bulk_ack(uint16_t id, uint32_t next, void*) {
  if (!s_out.active || id != s_out.id) return;
  s_out.acked = next;
  if (next < s_out.sent) {
    // 受理されていない部分を送り直す。
    s_out.sent = next;
    s_out.inflight = 0;
    s_out.end_sent = false;
  } else if (next > s_out.sent) {
    s_out.sent = next;  // ありえないが念のためクランプ
    s_out.inflight = 0;
  } else {
    s_out.inflight = 0;
  }
  s_out.retries = 0;
  s_out.last_ms = clock()->now_ms();
  wake_task();  // app タスクでポンプを進める
}

void bulk_out_pump() {
  if (!s_out.active) return;
  if (!ble_link_is_connected() || !fctx() || !fctx()->audio) {
    bulk_out_stop("link down");
    return;
  }
  const int64_t now = clock()->now_ms();
  // 進まないまましきい値超え → acked 位置から送り直す。
  if (now - s_out.last_ms > kBulkOutAckTimeoutMs &&
      (s_out.inflight > 0 || s_out.sent < s_out.size || s_out.end_sent)) {
    if (++s_out.retries > kBulkOutMaxRetry) {
      bulk_out_stop("ack timeout");
      return;
    }
    s_out.sent = s_out.acked;
    s_out.inflight = 0;
    s_out.end_sent = false;
    s_out.last_ms = now;
  }

  while (s_out.inflight < kBulkOutWindow && s_out.sent < s_out.size) {
    uint8_t frame[watch::proto::kBulkChunkHead + kBulkOutChunk];
    watch::proto::bulk_chunk_head(s_out.id, s_out.sent, frame);
    size_t len = kBulkOutChunk;
    if (s_out.sent + len > s_out.size) len = s_out.size - s_out.sent;
    size_t got = len;
    if (!fctx()->audio->memo_audio_read(s_out.file_id, s_out.sent,
                                        frame + watch::proto::kBulkChunkHead,
                                        &got) ||
        got == 0) {
      bulk_out_stop("read failed");
      return;
    }
    if (ble_link_bulk_send(0x11, s_out.id, frame,
                           watch::proto::kBulkChunkHead + got) != ESP_OK) {
      bulk_out_stop("send failed");
      return;
    }
    s_out.sent += got;
    ++s_out.inflight;
    s_out.last_ms = now;
    if (got < len) break;  // ファイル末尾
  }

  if (s_out.sent >= s_out.size && !s_out.end_sent) {
    uint8_t buf[32];
    const size_t n = watch::proto::bulk_encode_end(s_out.id, buf, sizeof(buf));
    if (n > 0 &&
        ble_link_bulk_send(0x13, s_out.id, buf, n) == ESP_OK) {
      s_out.end_sent = true;
      s_out.last_ms = now;
    }
  }
  if (s_out.end_sent && s_out.acked >= s_out.size) {
    ESP_LOGI(TAG, "bulk out done file=%lu",
             static_cast<unsigned long>(s_out.file_id));
    s_out.active = false;
    if (s_out.done) s_out.done(s_out.id, true);
  }
}

// ---- agent (AI) の送信 ----------------------------------------------------
// core が持つ「送りたい物」を app タスクのループで実際に出す。
// 音声は BULK kind="agent_audio"、定型質問は EVT agent.request。

void agent_bulk_done(uint16_t id, bool ok) {
  core_lock();
  watch::features::agent_sent(id, ok, *fctx());
  core_unlock();
}

void write_evt_agent_request(watch::cbor::Writer& w, void* ctx) {
  const auto& p = *static_cast<const watch::features::AgentPending*>(ctx);
  w.map(2).text("id").uint_v(p.id).text("text").text(p.text);
}

void agent_poll_send() {
  watch::features::AgentPending p;
  core_lock();
  const bool has = watch::features::agent_pending(&p);
  core_unlock();
  if (!has || !ble_link_is_connected()) {
    return;  // 未接続は feature の tick がエラーにする
  }
  if (p.audio) {
    // 録音 (ADP1) を BULK kind="agent_audio" で push。
    if (bulk_out_begin(p.file_id, "agent_audio", p.id, agent_bulk_done)) {
      core_lock();
      watch::features::agent_send_started(p.id);
      core_unlock();
    }
  } else {
    uint8_t buf[160];
    watch::cbor::Writer w(buf, sizeof(buf));
    if (watch::proto::encode_evt(&w, "agent.request",
                                 write_evt_agent_request, &p) &&
        w.ok() && ble_link_send_event(buf, w.size()) == ESP_OK) {
      core_lock();
      watch::features::agent_send_started(p.id);
      watch::features::agent_sent(p.id, true, *fctx());
      core_unlock();
    }
  }
}

// ---- EventBus → EVT notify -------------------------------------------------
// protocol-v1.md の EVT 表に対応する Event を送る。

void write_evt_battery(watch::cbor::Writer& w, void*) {
  w.map(2)
      .text("level")
      .int_v(board::pmic::battery_percent())
      .text("charging")
      .bool_v(board::pmic::is_charging());
}

void write_evt_memo_saved(watch::cbor::Writer& w, void* ctx) {
  const uint32_t id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ctx));
  watch::features::MemoEntry e;
  const bool found = watch::features::memo_find(id, &e);
  w.map(3)
      .text("id")
      .uint_v(id)
      .text("kind")
      .text(found && e.kind == watch::features::MemoKind::Voice ? "voice"
                                                              : "text")
      .text("sec")
      .uint_v(found ? e.sec : 0);
}

void write_evt_id(watch::cbor::Writer& w, void* ctx) {
  const uint32_t id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ctx));
  w.map(1).text("id").uint_v(id);
}

// OtaProgress Event 発行時点の ota::status() スナップショット。
ota::Status s_ota_snap;

void write_evt_ota_progress(watch::cbor::Writer& w, void*) {
  w.map(2)
      .text("pct")
      .uint_v(s_ota_snap.pct)
      .text("stage")
      .text(ota::stage_name(s_ota_snap.stage));
}

void write_evt_ota_result(watch::cbor::Writer& w, void*) {
  const bool ok = s_ota_snap.stage == ota::Stage::Done ||
                  s_ota_snap.stage == ota::Stage::Reboot;
  w.map(2).text("ok").bool_v(ok).text("msg").text(s_ota_snap.msg);
}

void on_bus_evt(const watch::Event& e, void*) {
  if (!ble_link_is_connected()) return;
  uint8_t buf[160];
  watch::cbor::Writer w(buf, sizeof(buf));
  bool ok = false;
  switch (e.type) {
    case watch::EventType::BatteryChanged:
    case watch::EventType::ChargingChanged:
      ok = watch::proto::encode_evt(&w, "battery", write_evt_battery, nullptr);
      break;
    case watch::EventType::TimerFinished:
      ok = watch::proto::encode_evt(&w, "timer.finished", nullptr, nullptr);
      break;
    case watch::EventType::MemoSaved:
      ok = watch::proto::encode_evt(
          &w, "memo.saved", write_evt_memo_saved,
          reinterpret_cast<void*>(static_cast<uintptr_t>(e.arg0)));
      break;
    case watch::EventType::MemoDeleted:
      ok = watch::proto::encode_evt(
          &w, "memo.deleted", write_evt_id,
          reinterpret_cast<void*>(static_cast<uintptr_t>(e.arg0)));
      break;
    case watch::EventType::OtaProgress:
      s_ota_snap = ota::status();
      // 終端ステージでは ota.result、途中は ota.progress。
      if (s_ota_snap.stage == ota::Stage::Done ||
          s_ota_snap.stage == ota::Stage::Fail ||
          s_ota_snap.stage == ota::Stage::Reboot) {
        ok = watch::proto::encode_evt(&w, "ota.result",
                                      write_evt_ota_result, nullptr);
      } else {
        ok = watch::proto::encode_evt(&w, "ota.progress",
                                      write_evt_ota_progress, nullptr);
      }
      break;
    default:
      break;
  }
  if (ok && w.ok()) {
    ble_link_send_event(buf, w.size());
  }
}

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
  svc.memo_count = [](void*) {
    return static_cast<int32_t>(watch::features::memo_count());
  };
  svc.memo_entry = [](uint32_t i, watch::cbor::Writer& w, void*) {
    const int32_t total =
        static_cast<int32_t>(watch::features::memo_count());
    watch::features::MemoEntry e;
    if (static_cast<int32_t>(i) >= total ||
        !watch::features::memo_at(total - 1 - static_cast<int32_t>(i), &e)) {
      return false;
    }
    w.map(4)
        .text("id")
        .uint_v(e.id)
        .text("kind")
        .text(e.kind == watch::features::MemoKind::Voice ? "voice" : "text")
        .text("sec")
        .uint_v(e.sec)
        .text("size")
        .uint_v(e.size);
    return true;
  };
  svc.memo_get = [](uint32_t id, watch::cbor::Writer& w, void*) {
    watch::features::MemoEntry e;
    if (!watch::features::memo_find(id, &e)) return false;
    w.map(5)
        .text("id")
        .uint_v(e.id)
        .text("kind")
        .text(e.kind == watch::features::MemoKind::Voice ? "voice" : "text")
        .text("sec")
        .uint_v(e.sec)
        .text("size")
        .uint_v(e.size)
        .text("text")
        .text(e.text);
    return true;
  };
  svc.memo_delete = [](uint32_t id, void* c) -> int32_t {
    return watch::features::memo_delete(
               id, *static_cast<watch::FeatureContext*>(c))
               ? 1
               : 0;
  };
  svc.memo_audio_info = [](uint32_t id, uint32_t* size, uint8_t sha[32],
                           void* c) {
    watch::FeatureContext* f = static_cast<watch::FeatureContext*>(c);
    watch::features::MemoEntry e;
    if (!f->audio || !watch::features::memo_find(id, &e) ||
        e.kind != watch::features::MemoKind::Voice ||
        !f->audio->memo_audio_size(id, size)) {
      return false;
    }
    watch::proto::Sha256 h;
    uint8_t buf[256];
    uint32_t off = 0;
    for (;;) {
      size_t len = sizeof(buf);
      if (!f->audio->memo_audio_read(id, off, buf, &len)) return false;
      if (len == 0) break;
      h.update(buf, len);
      off += len;
    }
    h.finish(sha);
    return true;
  };
  svc.memo_audio_send = [](uint32_t id, void*) {
    return bulk_out_begin(id, "memo", static_cast<uint16_t>(id & 0xFFFF),
                          nullptr);
  };
  svc.agent_reply = [](uint16_t id, const char* text, size_t len, void* c) {
    return watch::features::agent_on_reply(
        id, text, len, *static_cast<watch::FeatureContext*>(c));
  };
  svc.steps_today = [](void*) {
    return watch::features::steps_today();
  };
  svc.ctx = fctx();
  // settings.set {theme:...} → 適用待ちに登録 (app タスクが SetTheme を投げる)。
  svc.setting_changed = [](const char* key, void*) {
    if (std::strcmp(key, "theme") == 0) {
      theme_store::set_pending_theme(settings_mut().theme);
    }
  };
  svc.wifi_set = [](const char* ssid, const char* pass, void*) {
    return wifi::set_credentials(ssid, pass);
  };
  svc.wifi_info = [](char* out, size_t cap, void*) {
    return wifi::ssid(out, cap);
  };
  svc.ota_start = [](const char* url, const uint8_t sha[32], const char* ver,
                     void*) {
    return ota::start_https(url, sha, ver);
  };
  svc.ota_status = [](watch::cbor::Writer& w, void*) {
    const ota::Status s = ota::status();
    const bool active =
        s.stage != ota::Stage::Idle && s.stage != ota::Stage::Fail;
    w.map(5)
        .text("active")
        .bool_v(active)
        .text("stage")
        .text(ota::stage_name(s.stage))
        .text("pct")
        .uint_v(s.pct)
        .text("msg")
        .text(s.msg)
        .text("version")
        .text(s.version);
    return true;
  };

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

// ---- BULK 受信の振り分け -----------------------------------------------------
// kind は BULK_START でしか届かないので begin 時に路線を固定する。
// "firmware" は ota (ファーム更新)、それ以外は theme_store (theme/asset/memo)。
enum class BulkRoute : uint8_t { None, Theme, Firmware };
BulkRoute s_bulk_route = BulkRoute::None;

bool bulk_begin_tr(uint16_t id, const char* kind, uint32_t size, void*) {
  const bool fw = std::strcmp(kind, "firmware") == 0;
  const bool ok = fw ? ota::bulk_begin(id, size)
                     : theme_store::bulk_begin(id, kind, size, nullptr);
  s_bulk_route = ok ? (fw ? BulkRoute::Firmware : BulkRoute::Theme)
                    : BulkRoute::None;
  return ok;
}

bool bulk_write_tr(uint16_t id, uint32_t offset, const uint8_t* data,
                   size_t len, void*) {
  return s_bulk_route == BulkRoute::Firmware
             ? ota::bulk_write(id, offset, data, len)
             : theme_store::bulk_write(id, offset, data, len, nullptr);
}

bool bulk_commit_tr(uint16_t id, void*) {
  const BulkRoute r = s_bulk_route;
  s_bulk_route = BulkRoute::None;
  return r == BulkRoute::Firmware ? ota::bulk_commit(id)
                                  : theme_store::bulk_commit(id, nullptr);
}

void bulk_abort_tr(uint16_t id, void*) {
  const BulkRoute r = s_bulk_route;
  s_bulk_route = BulkRoute::None;
  if (r == BulkRoute::Firmware) {
    ota::bulk_abort(id);
  } else {
    theme_store::bulk_abort(id, nullptr);
  }
}

ble_link_config_t s_cfg = {
    .dispatch_req = ble_dispatch,
    .dispatch_ctx = nullptr,
    .on_conn_state = on_conn_state,
    .on_passkey = on_passkey,
    .cb_ctx = nullptr,
    .bulk_begin = bulk_begin_tr,
    .bulk_write = bulk_write_tr,
    .bulk_commit = bulk_commit_tr,
    .bulk_abort = bulk_abort_tr,
    .bulk_ctx = nullptr,
    .on_bulk_ack = on_bulk_ack,
};

}  // namespace

const ble_link_config_t* ble_config() { return &s_cfg; }

// watch_app::start から1回呼ぶ。EventBus → EVT notify を配線する。
void ble_glue_init() {
  bus().subscribe(watch::EventType::BatteryChanged, on_bus_evt, nullptr);
  bus().subscribe(watch::EventType::ChargingChanged, on_bus_evt, nullptr);
  bus().subscribe(watch::EventType::TimerFinished, on_bus_evt, nullptr);
  bus().subscribe(watch::EventType::MemoSaved, on_bus_evt, nullptr);
  bus().subscribe(watch::EventType::MemoDeleted, on_bus_evt, nullptr);
  bus().subscribe(watch::EventType::OtaProgress, on_bus_evt, nullptr);
}

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
    if (conn == 0 && s_out.active) {
      bulk_out_stop("disconnected");
    }
  }
  const int32_t key = s_pending_key;
  if (key >= 0) {
    s_pending_key = -1;
    ui::request_passkey(static_cast<uint32_t>(key));
  }
  agent_poll_send();
  bulk_out_pump();
  // テーマ適用待ち (BULK 受信 or settings.set) → SetTheme Action で適用。
  // 画面OFF中でも効くよう source=System (外部入力は dispatch で捨てられる)。
  char theme_id[32];
  if (theme_store::take_pending_theme(theme_id, sizeof(theme_id))) {
    watch::Action a{};
    a.type = watch::ActionType::SetTheme;
    a.source = watch::ActionSource::System;
    a.set_text(theme_id);
    push_action(a);
  }
}

}  // namespace watch_app

#else  // !CONFIG_SAS_BLE_LINK

namespace watch_app {
void ble_glue_init() {}
void ble_glue_poll() {}
}  // namespace watch_app

#endif
