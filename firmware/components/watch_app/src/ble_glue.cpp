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
#include "watch/features/alarm.hpp"
#include "watch/features/media.hpp"
#include "wifi/wifi.hpp"
#include "watch/features/memo.hpp"
#include "watch/features/notify.hpp"
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
// settings.set {face:...} / {clock_font:...} → app タスクで Action 化。
char s_pending_face[16] = {};
char s_pending_font[16] = {};

// ---- bulk_out (音声メモの時計→スマホ送信) ----------------------------------
// Phone は受信側として BULK_ACK{next} を返す (8チャンクごと/END後)。
// ACK が来ない/進まない部分はタイムアウトで acked 位置から送り直す。
struct BulkOut {
  bool active = false;
  uint16_t id = 0;        // BULK の transfer id (= memo_id & 0xFFFF)
  uint32_t memo_id = 0;
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
}

bool bulk_out_begin(uint32_t memo_id) {
  if (s_out.active || !ble_link_bulk_ready() || !fctx() || !fctx()->audio) {
    return false;
  }
  uint32_t size = 0;
  if (!fctx()->audio->memo_audio_size(memo_id, &size) || size == 0) {
    return false;
  }
  // ファイル全体の sha256 を流しながら計算する。
  watch::proto::Sha256 h;
  uint8_t buf[256];
  uint32_t off = 0;
  for (;;) {
    size_t len = sizeof(buf);
    if (!fctx()->audio->memo_audio_read(memo_id, off, buf, &len)) return false;
    if (len == 0) break;
    h.update(buf, len);
    off += len;
  }
  if (off != size) return false;
  uint8_t sha[32];
  h.finish(sha);

  uint8_t start[96];
  const uint16_t tid = static_cast<uint16_t>(memo_id & 0xFFFF);
  const size_t n = watch::proto::bulk_encode_start(
      tid, "memo", size, sha, kBulkOutChunk, start, sizeof(start));
  if (n == 0) return false;
  if (ble_link_bulk_send(0x10, tid, start, n) != ESP_OK) return false;
  s_out = BulkOut{};
  s_out.active = true;
  s_out.id = tid;
  s_out.memo_id = memo_id;
  s_out.size = size;
  s_out.last_ms = clock()->now_ms();
  ESP_LOGI(TAG, "bulk out start memo=%lu size=%lu",
           static_cast<unsigned long>(memo_id),
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
    if (!fctx()->audio->memo_audio_read(s_out.memo_id, s_out.sent,
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
    ESP_LOGI(TAG, "bulk out done memo=%lu",
             static_cast<unsigned long>(s_out.memo_id));
    s_out.active = false;
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

// media.cmd の数値 → ワイヤ表現 (protocol-v1.md と一致させる)。
const char* media_cmd_wire(uint32_t cmd) {
  switch (static_cast<watch::MediaCmd>(cmd)) {
    case watch::MediaCmd::PlayPause: return "play_pause";
    case watch::MediaCmd::Next: return "next";
    case watch::MediaCmd::Prev: return "prev";
    case watch::MediaCmd::VolUp: return "vol_up";
    case watch::MediaCmd::VolDown: return "vol_down";
    default: return nullptr;
  }
}

void write_evt_media_cmd(watch::cbor::Writer& w, void* ctx) {
  const uint32_t cmd = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ctx));
  const char* s = media_cmd_wire(cmd);
  w.map(1).text("cmd").text(s ? s : "unknown");
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
    case watch::EventType::AlarmRinging:
      ok = watch::proto::encode_evt(
          &w, "alarm.ringing", write_evt_id,
          reinterpret_cast<void*>(static_cast<uintptr_t>(e.arg0)));
      break;
    case watch::EventType::MediaCmdRequested:
      ok = watch::proto::encode_evt(
          &w, "media.cmd", write_evt_media_cmd,
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
    return bulk_out_begin(id);
  };
  svc.alarm_count = [](void*) {
    return static_cast<int32_t>(watch::features::alarm_count());
  };
  svc.alarm_entry = [](uint32_t i, watch::cbor::Writer& w, void*) {
    watch::features::AlarmEntry e;
    if (!watch::features::alarm_at(i, &e)) return false;
    w.map(5)
        .text("id")
        .uint_v(e.id)
        .text("hour")
        .uint_v(e.hour)
        .text("min")
        .uint_v(e.min)
        .text("dow")
        .uint_v(e.dow)
        .text("on")
        .bool_v(e.enabled != 0);
    return true;
  };
  svc.alarm_set = [](uint32_t id, uint8_t hour, uint8_t min, uint8_t dow,
                     bool on, void* c) -> int32_t {
    return watch::features::alarm_set(
        id, hour, min, dow, on, *static_cast<watch::FeatureContext*>(c));
  };
  svc.alarm_delete = [](uint32_t id, void* c) -> int32_t {
    return watch::features::alarm_delete(
        id, *static_cast<watch::FeatureContext*>(c));
  };
  svc.notify_posted = [](const char* app, const char* title, const char* body,
                         void*) {
    watch::features::notify_add(app, title, body);
  };
  svc.media_state = [](const char* title, const char* artist, bool playing,
                       void* c) {
    watch::features::media_set(title, artist, playing,
                               *static_cast<watch::FeatureContext*>(c));
  };
  svc.steps_today = [](void*) {
    return watch::features::steps_today();
  };
  svc.ctx = fctx();
  // settings.set {theme:...} → 適用待ちに登録 (app タスクが SetTheme を投げる)。
  // face / clock_font も同じ経路 (dispatch 側で既に Settings へ保存済み、
  // ここでは再描画用の Action だけ保留する)。
  svc.setting_changed = [](const char* key, void*) {
    if (std::strcmp(key, "theme") == 0) {
      theme_store::set_pending_theme(settings_mut().theme);
    } else if (std::strcmp(key, "face") == 0) {
      std::strncpy(s_pending_face, settings_mut().face,
                   sizeof(s_pending_face) - 1);
      s_pending_face[sizeof(s_pending_face) - 1] = '\0';
    } else if (std::strcmp(key, "clock_font") == 0) {
      std::strncpy(s_pending_font, settings_mut().clock_font,
                   sizeof(s_pending_font) - 1);
      s_pending_font[sizeof(s_pending_font) - 1] = '\0';
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
  bus().subscribe(watch::EventType::AlarmRinging, on_bus_evt, nullptr);
  bus().subscribe(watch::EventType::MediaCmdRequested, on_bus_evt, nullptr);
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
  // 文字盤・数字フォント (settings.set 経由) → Action で再描画。
  if (s_pending_face[0]) {
    watch::Action a{};
    a.type = watch::ActionType::SetFace;
    a.source = watch::ActionSource::System;
    a.set_text(s_pending_face);
    s_pending_face[0] = '\0';
    push_action(a);
  }
  if (s_pending_font[0]) {
    watch::Action a{};
    a.type = watch::ActionType::SetClockFont;
    a.source = watch::ActionSource::System;
    a.set_text(s_pending_font);
    s_pending_font[0] = '\0';
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
