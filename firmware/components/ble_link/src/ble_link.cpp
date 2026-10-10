// ble_link — NimBLE GATT サーバ (docs/protocol-v1.md)
//
// スレッドモデル:
//   - GATT コールバックと送信は全部 NimBLE host タスクの中で動く。
//   - ble_link_send_event() / ble_link_confirm_passkey() は他タスクから
//     呼ばれる前提 (NimBLE 側が内部でロックを取る)。
//   - BULK の受信状態は切断をまたいで保持する (再送で再開できる)。
#include "ble_link.h"

#include <cstdio>
#include <cstring>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "nimble/ble.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_id.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "watch/protocol/bulk.hpp"
#include "watch/protocol/cbor.hpp"
#include "watch/protocol/frame.hpp"

namespace {

constexpr char kTag[] = "ble_link";

// protocol-v1.md の UUID (128bit は BLE の wire 順 = 文字列表記の逆順)。
constexpr ble_uuid128_t kSvcUuid = BLE_UUID128_INIT(
    0xe1, 0xa7, 0xc0, 0x11, 0x0a, 0x6f, 0x3f, 0x9c,
    0x8e, 0x4b, 0x2b, 0x5d, 0x01, 0x00, 0x1e, 0x7a);
constexpr ble_uuid128_t kChrCtrlUuid = BLE_UUID128_INIT(
    0xe1, 0xa7, 0xc0, 0x11, 0x0a, 0x6f, 0x3f, 0x9c,
    0x8e, 0x4b, 0x2b, 0x5d, 0x02, 0x00, 0x1e, 0x7a);
constexpr ble_uuid128_t kChrEventUuid = BLE_UUID128_INIT(
    0xe1, 0xa7, 0xc0, 0x11, 0x0a, 0x6f, 0x3f, 0x9c,
    0x8e, 0x4b, 0x2b, 0x5d, 0x03, 0x00, 0x1e, 0x7a);
constexpr ble_uuid128_t kChrBulkUuid = BLE_UUID128_INIT(
    0xe1, 0xa7, 0xc0, 0x11, 0x0a, 0x6f, 0x3f, 0x9c,
    0x8e, 0x4b, 0x2b, 0x5d, 0x04, 0x00, 0x1e, 0x7a);

// Advertising / 接続パラメータ (plan.md: 省電力)。
constexpr uint16_t kAdvItvlMin = 800;   // 500ms (単位 0.625ms)
constexpr uint16_t kAdvItvlMax = 1600;  // 1000ms
constexpr uint16_t kConnItvlMin = 80;   // 100ms (単位 1.25ms)
constexpr uint16_t kConnItvlMax = 160;  // 200ms
constexpr uint16_t kConnLatency = 4;    // slave latency
constexpr uint16_t kSupervisionTimeout = 500;  // 5s (単位 10ms)

ble_link_config_t s_cfg;
bool s_started = false;
uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
uint16_t s_mtu = BLE_ATT_MTU_DFLT;
uint8_t s_addr_type = 0;
uint16_t s_evt_msg_id = 1;
uint16_t s_bulk_tx_msg_id = 1;  // 時計→スマホ方向の BULK の連番
uint16_t s_passkey_conn = BLE_HS_CONN_HANDLE_NONE;  // numcmp 確認待ち

// char の値ハンドル (ble_gatts_add_svcs が埋める)。
uint16_t s_ctrl_val_handle;
uint16_t s_event_val_handle;
uint16_t s_bulk_val_handle;
// 各 char の通知購読状態。
bool s_ctrl_notify;
bool s_event_notify;
bool s_bulk_notify;

watch::proto::Reassembler s_ctrl_rx;  // REQ の組み立て
watch::proto::Reassembler s_bulk_rx_frames;  // BULK_* の組み立て
watch::proto::BulkReceiver s_bulk;  // BULK 転送状態 (切断をまたぐ)

// light sleep 抑止用の PM lock (ESP_PM_NO_LIGHT_SLEEP)。
// ESP32-S3 の BLE は modem sleep 非対応で BLE wakeup source も無いため、
// 接続中に light sleep へ入るとリンクが壊れる。
//   s_no_ls_conn: BLE 接続中は常時保持 (CONNECT〜DISCONNECT)
//   s_no_ls_bulk: BULK 受信の進行中だけ重ねて保持 (切断したら一旦解放)
esp_pm_lock_handle_t s_no_ls_conn = nullptr;
esp_pm_lock_handle_t s_no_ls_bulk = nullptr;
bool s_conn_locked = false;
bool s_bulk_locked = false;

// 接続/転送の状態に合わせて NO_LIGHT_SLEEP lock を対称に出し入れする。
// (CONFIG_PM_ENABLE 無しでは handle が NULL のままなので no-op で安全)
void conn_lock_sync() {
  const bool want = s_conn != BLE_HS_CONN_HANDLE_NONE;
  if (want == s_conn_locked || !s_no_ls_conn) return;
  if (want) {
    esp_pm_lock_acquire(s_no_ls_conn);
  } else {
    esp_pm_lock_release(s_no_ls_conn);
  }
  s_conn_locked = want;
}

// BULK 受信の進行中だけ lock を重ねる。転送状態は切断をまたいで残るが、
// 切断中は進めないので一旦解放し、再接続後の再送 (BULK_START/CHUNK) で
// 再取得する。BulkReceiver の状態を変える箇所の最後に呼ぶ。
void bulk_lock_sync() {
  const bool want =
      s_conn != BLE_HS_CONN_HANDLE_NONE && s_bulk.in_progress();
  if (want == s_bulk_locked || !s_no_ls_bulk) return;
  if (want) {
    esp_pm_lock_acquire(s_no_ls_bulk);
  } else {
    esp_pm_lock_release(s_no_ls_bulk);
  }
  s_bulk_locked = want;
}

// ---------- 送信 ----------

// send_message() の emit: frame を char ハンドル attr に notify する。
bool notify_emit(const uint8_t* frame, size_t len, void* ctx) {
  const uint16_t attr = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(ctx));
  if (s_conn == BLE_HS_CONN_HANDLE_NONE) return false;
  os_mbuf* om = ble_hs_mbuf_from_flat(frame, len);
  if (!om) return false;
  return ble_gatts_notify_custom(s_conn, attr, om) == 0;
}

void send_res_payload(uint16_t msg_id, const uint8_t* payload, size_t len) {
  watch::proto::send_message(watch::proto::FrameType::Res, msg_id, payload, len,
                             s_mtu, notify_emit,
                             reinterpret_cast<void*>(s_ctrl_val_handle));
}

size_t encode_res_ok(uint8_t* out, size_t cap) {
  watch::cbor::Writer w(out, cap);
  w.map(2).text("ok").bool_v(true).text("r").map(0);
  return w.ok() ? w.size() : 0;
}

size_t encode_res_err(uint8_t* out, size_t cap, const char* code) {
  watch::cbor::Writer w(out, cap);
  w.map(3).text("ok").bool_v(false).text("e").text(code).text("msg").text("");
  return w.ok() ? w.size() : 0;
}

void send_res_err(uint16_t msg_id, const char* code) {
  uint8_t buf[64];
  send_res_payload(msg_id, buf, encode_res_err(buf, sizeof(buf), code));
}

void send_bulk_ack(uint16_t id, uint32_t next) {
  uint8_t payload[32];
  const size_t n = watch::proto::bulk_encode_ack(id, next, payload,
                                                 sizeof(payload));
  if (n == 0) return;
  watch::proto::send_message(watch::proto::FrameType::BulkAck, id, payload, n,
                             s_mtu, notify_emit,
                             reinterpret_cast<void*>(s_bulk_val_handle));
}

// ---------- 接続状態 ----------

bool conn_is_authenticated() {
  ble_gap_conn_desc d;
  if (s_conn == BLE_HS_CONN_HANDLE_NONE ||
      ble_gap_conn_find(s_conn, &d) != 0) {
    return false;
  }
  return d.sec_state.authenticated != 0;
}

void notify_conn_state(bool connected) {
  if (s_cfg.on_conn_state) {
    s_cfg.on_conn_state(connected, connected && conn_is_authenticated(),
                        s_cfg.cb_ctx);
  }
}

// ---------- GATT: ctrl ----------

int ctrl_access(uint16_t conn_handle, uint16_t attr_handle,
                ble_gatt_access_ctxt* ctxt, void* arg) {
  if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
    return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
  }
  uint8_t buf[watch::proto::kDefaultMtu + 16];
  uint16_t len = 0;
  if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) {
    return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
  }

  watch::proto::Frame f;
  if (watch::proto::frame_parse(buf, len, &f) != watch::proto::FrameError::Ok ||
      f.type != watch::proto::FrameType::Req) {
    ESP_LOGW(kTag, "ctrl: bad frame (%d bytes)", len);
    return BLE_ATT_ERR_UNLIKELY;
  }

  watch::proto::Frame req;
  if (!s_ctrl_rx.feed(f, &req)) {
    if (s_ctrl_rx.error()) {
      ESP_LOGW(kTag, "ctrl: reassembly error, reset");
      s_ctrl_rx.reset();
      return BLE_ATT_ERR_UNLIKELY;
    }
    return 0;  // 続きのフラグメント待ち
  }

  // 完成した REQ を dispatch して RES を返す。
  uint8_t res_buf[watch::proto::kMaxMessage];
  size_t res_len = 0;
  if (s_cfg.dispatch_req) {
    res_len = s_cfg.dispatch_req(req.payload, req.payload_len, res_buf,
                                 sizeof(res_buf), s_cfg.dispatch_ctx);
  }
  if (res_len == 0) {
    res_len = encode_res_err(res_buf, sizeof(res_buf), "busy");
  }
  send_res_payload(req.msg_id, res_buf, res_len);
  return 0;
}

// ---------- GATT: bulk ----------

void handle_bulk_msg(const watch::proto::Frame& msg) {
  uint32_t next = 0;
  bool ack_due = false;
  switch (msg.type) {
    case watch::proto::FrameType::BulkStart: {
      watch::proto::BulkStart bs;
      if (!watch::proto::bulk_parse_start(msg.payload, msg.payload_len, &bs)) {
        send_res_err(msg.msg_id, "bad_request");
        break;
      }
      if (!s_bulk.start(bs, &next)) {
        send_res_err(msg.msg_id, "busy");
        break;
      }
      // 新規は next=0、再送(再開)は途中位置が返る。
      send_bulk_ack(bs.id, next);
      break;
    }
    case watch::proto::FrameType::BulkChunk:
      if (s_bulk.feed_chunk(msg.payload, msg.payload_len, &next, &ack_due) &&
          ack_due) {
        send_bulk_ack(s_bulk.current_id(), next);
      }
      break;
    case watch::proto::FrameType::BulkEnd: {
      uint16_t id = 0;
      if (watch::proto::bulk_parse_end(msg.payload, msg.payload_len, &id) &&
          s_bulk.finish(id)) {
        uint8_t buf[32];
        send_res_payload(msg.msg_id, buf, encode_res_ok(buf, sizeof(buf)));
      } else {
        send_res_err(msg.msg_id, "bad_request");
      }
      break;
    }
    case watch::proto::FrameType::BulkAck: {
      // 時計→スマホ方向の転送で Phone が返す ACK。
      uint16_t id = 0;
      uint32_t next = 0;
      if (watch::proto::bulk_parse_ack(msg.payload, msg.payload_len, &id,
                                     &next) &&
          s_cfg.on_bulk_ack) {
        s_cfg.on_bulk_ack(id, next, s_cfg.bulk_ctx);
      }
      break;
    }
    default:
      break;
  }
  bulk_lock_sync();  // 転送の開始/完了/中断を反映
}

int bulk_access(uint16_t conn_handle, uint16_t attr_handle,
                ble_gatt_access_ctxt* ctxt, void* arg) {
  // write-without-response は ATT エラーを返せないので、
  // 壊れたフレームは捨てるだけ。
  if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
  uint8_t buf[watch::proto::kDefaultMtu + 16];
  uint16_t len = 0;
  if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) return 0;

  watch::proto::Frame f;
  if (watch::proto::frame_parse(buf, len, &f) !=
      watch::proto::FrameError::Ok) {
    return 0;
  }
  watch::proto::Frame msg;
  if (s_bulk_rx_frames.feed(f, &msg)) {
    handle_bulk_msg(msg);
  } else if (s_bulk_rx_frames.error()) {
    s_bulk_rx_frames.reset();
  }
  return 0;
}

// ---------- GATT テーブル ----------

// ctrl/bulk の write は BLE_GATT_CHR_F_WRITE_AUTHEN: ボンド済み(MITM認証済み)
// 以外の書き込みは NimBLE が insufficient authentication で拒否する
// (protocol-v1.md「ボンド済み端末以外の ctrl 書き込みは拒否」)。
ble_gatt_chr_def kSasChrs[] = {
    {
        .uuid = &kChrCtrlUuid.u,
        .access_cb = ctrl_access,
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY |
                 BLE_GATT_CHR_F_WRITE_AUTHEN,
        .min_key_size = 0,
        .val_handle = &s_ctrl_val_handle,
        .cpfd = nullptr,
    },
    {
        .uuid = &kChrEventUuid.u,
        .access_cb = nullptr,  // notify 専用
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_NOTIFY,
        .min_key_size = 0,
        .val_handle = &s_event_val_handle,
        .cpfd = nullptr,
    },
    {
        .uuid = &kChrBulkUuid.u,
        .access_cb = bulk_access,
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY |
                 BLE_GATT_CHR_F_WRITE_AUTHEN,
        .min_key_size = 0,
        .val_handle = &s_bulk_val_handle,
        .cpfd = nullptr,
    },
    {},
};

ble_gatt_svc_def kGattSvcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kSvcUuid.u,
        .includes = nullptr,
        .characteristics = kSasChrs,
    },
    {},
};

// ---------- GAP ----------

int gap_event(ble_gap_event* event, void* arg);

int start_advertising() {
  // "SAS-Watch-XXXX" の XXXX は MAC の下位 2 バイト (局所的な識別用)。
  uint8_t mac[6] = {0};
  ble_hs_id_copy_addr(s_addr_type, mac, nullptr);
  char name[16];
  snprintf(name, sizeof(name), "SAS-Watch-%02X%02X", mac[0], mac[1]);
  ble_svc_gap_device_name_set(name);

  // 31B 上限の関係で: adv = flags + service UUID、名前は scan response。
  ble_hs_adv_fields adv = {};
  adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  adv.uuids128 = &kSvcUuid;
  adv.num_uuids128 = 1;
  adv.uuids128_is_complete = 1;
  ble_gap_adv_set_fields(&adv);

  ble_hs_adv_fields rsp = {};
  rsp.name = reinterpret_cast<const uint8_t*>(name);
  rsp.name_len = strlen(name);
  rsp.name_is_complete = 1;
  ble_gap_adv_rsp_set_fields(&rsp);

  ble_gap_adv_params params = {};
  params.conn_mode = BLE_GAP_CONN_MODE_UND;
  params.disc_mode = BLE_GAP_DISC_MODE_GEN;
  params.itvl_min = kAdvItvlMin;
  params.itvl_max = kAdvItvlMax;
  const int rc = ble_gap_adv_start(s_addr_type, nullptr, BLE_HS_FOREVER,
                                   &params, gap_event, nullptr);
  if (rc != 0 && rc != BLE_HS_EALREADY) {
    ESP_LOGW(kTag, "adv start failed: %d", rc);
  }
  return rc;
}

void request_conn_params(uint16_t conn) {
  // 時計側が望む接続間隔を申し出る (採否は central 次第)。
  ble_gap_upd_params p = {};
  p.itvl_min = kConnItvlMin;
  p.itvl_max = kConnItvlMax;
  p.latency = kConnLatency;
  p.supervision_timeout = kSupervisionTimeout;
  p.min_ce_len = 0;
  p.max_ce_len = 0;
  const int rc = ble_gap_update_params(conn, &p);
  if (rc != 0) ESP_LOGW(kTag, "conn param update: %d", rc);
}

int gap_event(ble_gap_event* event, void* arg) {
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status == 0) {
        s_conn = event->connect.conn_handle;
        conn_lock_sync();  // 接続中は light sleep 禁止
        s_mtu = BLE_ATT_MTU_DFLT;
        s_ctrl_rx.reset();
        s_bulk_rx_frames.reset();
        s_ctrl_notify = s_event_notify = s_bulk_notify = false;
        ESP_LOGI(kTag, "connected (handle=%d)", s_conn);
        notify_conn_state(true);
        request_conn_params(s_conn);
        // MTU 交換をこちらからも要求 (protocol: 247 を要求)。
        ble_gattc_exchange_mtu(s_conn, nullptr, nullptr);
        // 前回切断またぎの転送が残っていれば再開に備えて lock を張り直す。
        bulk_lock_sync();
      } else {
        start_advertising();
      }
      break;

    case BLE_GAP_EVENT_DISCONNECT:
      ESP_LOGI(kTag, "disconnected (reason=0x%02x)", event->disconnect.reason);
      s_conn = BLE_HS_CONN_HANDLE_NONE;
      conn_lock_sync();  // 接続 lock を解放
      bulk_lock_sync();  // 転送中でも切断中は進めないので解放
      s_mtu = BLE_ATT_MTU_DFLT;
      s_passkey_conn = BLE_HS_CONN_HANDLE_NONE;
      s_ctrl_rx.reset();
      s_bulk_rx_frames.reset();
      // BULK の転送状態は残す: Phone が BULK_START を再送して再開する。
      notify_conn_state(false);
      start_advertising();
      break;

    case BLE_GAP_EVENT_CONN_UPDATE:
      if (event->conn_update.status == 0) {
        ble_gap_conn_desc d;
        if (ble_gap_conn_find(s_conn, &d) == 0) {
          ESP_LOGI(kTag, "conn params: itvl=%ums latency=%u",
                   (unsigned)(d.conn_itvl * 5 / 4),
                   (unsigned)d.conn_latency);
        }
      }
      break;

    case BLE_GAP_EVENT_MTU:
      s_mtu = event->mtu.value;
      ESP_LOGI(kTag, "mtu=%u", s_mtu);
      break;

    case BLE_GAP_EVENT_SUBSCRIBE:
      if (event->subscribe.attr_handle == s_ctrl_val_handle) {
        s_ctrl_notify = event->subscribe.cur_notify;
      } else if (event->subscribe.attr_handle == s_event_val_handle) {
        s_event_notify = event->subscribe.cur_notify;
      } else if (event->subscribe.attr_handle == s_bulk_val_handle) {
        s_bulk_notify = event->subscribe.cur_notify;
      }
      break;

    case BLE_GAP_EVENT_ENC_CHANGE:
      // 暗号化状態が変わった。authenticated になったタイミングを UI に伝える。
      if (event->enc_change.status == 0) {
        ESP_LOGI(kTag, "link encrypted (authenticated=%d)",
                 conn_is_authenticated());
        notify_conn_state(true);
      }
      break;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
      if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
        s_passkey_conn = event->passkey.conn_handle;
        if (s_cfg.on_passkey) {
          // 6桁を画面に出して、UI の確定を ble_link_confirm_passkey() で受ける。
          s_cfg.on_passkey(event->passkey.params.numcmp, s_cfg.cb_ctx);
        } else {
          // 表示経路が無いのに自動YESは危険なので拒否する。
          ble_sm_io io = {};
          io.action = BLE_SM_IOACT_NUMCMP;
          io.numcmp_accept = 0;
          ble_sm_inject_io(s_passkey_conn, &io);
          s_passkey_conn = BLE_HS_CONN_HANDLE_NONE;
        }
      }
      break;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
      // 再ペアリング要求: 旧ボンドを消してやり直す (bleprph と同じ作法)。
      ble_gap_conn_desc desc;
      if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
        ble_store_util_delete_peer(&desc.peer_id_addr);
      }
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    default:
      break;
  }
  return 0;
}

// ---------- BULK sink → config への橋渡し ----------

bool bulk_begin_tr(uint16_t id, const char* kind, uint32_t size, void* ctx) {
  const ble_link_config_t* c = static_cast<const ble_link_config_t*>(ctx);
  return !c->bulk_begin || c->bulk_begin(id, kind, size, c->bulk_ctx);
}
bool bulk_write_tr(uint16_t id, uint32_t offset, const uint8_t* data,
                   size_t len, void* ctx) {
  const ble_link_config_t* c = static_cast<const ble_link_config_t*>(ctx);
  return !c->bulk_write || c->bulk_write(id, offset, data, len, c->bulk_ctx);
}
bool bulk_commit_tr(uint16_t id, void* ctx) {
  const ble_link_config_t* c = static_cast<const ble_link_config_t*>(ctx);
  return !c->bulk_commit || c->bulk_commit(id, c->bulk_ctx);
}
void bulk_abort_tr(uint16_t id, void* ctx) {
  const ble_link_config_t* c = static_cast<const ble_link_config_t*>(ctx);
  if (c->bulk_abort) c->bulk_abort(id, c->bulk_ctx);
}

void on_sync() {
  // アドレスタイプが確定してから advertising を始める。
  ble_hs_id_infer_auto(0, &s_addr_type);
  start_advertising();
}

void on_reset(int reason) {
  ESP_LOGW(kTag, "nimble reset (reason=%d)", reason);
}

void host_task(void*) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}

}  // namespace

// ---------- 公開 API ----------

extern "C" esp_err_t ble_link_start(const ble_link_config_t* cfg) {
  if (s_started) return ESP_ERR_INVALID_STATE;
  if (!cfg) return ESP_ERR_INVALID_ARG;

  s_cfg = *cfg;
  watch::proto::BulkReceiver::Sink sink;
  sink.begin = bulk_begin_tr;
  sink.write = bulk_write_tr;
  sink.commit = bulk_commit_tr;
  sink.abort = bulk_abort_tr;
  sink.ctx = &s_cfg;
  s_bulk.init(sink);
  s_ctrl_rx.reset();
  s_bulk_rx_frames.reset();

  // light sleep 抑止の PM lock を起動時に確保しておく (acquire/release は
  // 実行時に対称に行う)。確保は初期化時のみのルール。
  esp_err_t prc = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP,
                                     ESP_PM_CPU_FREQ_MAX, "ble_conn",
                                     &s_no_ls_conn);
  if (prc != ESP_OK) {
    ESP_LOGW(kTag, "pm lock create (conn): %s", esp_err_to_name(prc));
  }
  prc = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, ESP_PM_CPU_FREQ_MAX,
                           "ble_bulk", &s_no_ls_bulk);
  if (prc != ESP_OK) {
    ESP_LOGW(kTag, "pm lock create (bulk): %s", esp_err_to_name(prc));
  }

  esp_err_t rc = nimble_port_init();
  if (rc != ESP_OK) return rc;

  ble_hs_cfg.reset_cb = on_reset;
  ble_hs_cfg.sync_cb = on_sync;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

  // LE Secure Connections + Bonding + Numeric Comparison (DisplayYesNo)。
  ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_YESNO;
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_mitm = 1;
  ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_our_key_dist =
      BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist =
      BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

  ble_svc_gap_init();
  ble_svc_gatt_init();
  ble_att_set_preferred_mtu(watch::proto::kDefaultMtu);

  int r = ble_gatts_count_cfg(kGattSvcs);
  if (r != 0) return ESP_FAIL;
  r = ble_gatts_add_svcs(kGattSvcs);
  if (r != 0) return ESP_FAIL;
  r = ble_gatts_start();
  if (r != 0) return ESP_FAIL;

  nimble_port_freertos_init(host_task);
  s_started = true;
  ESP_LOGI(kTag, "ble_link started");
  return ESP_OK;
}

extern "C" esp_err_t ble_link_stop(void) {
  if (!s_started) return ESP_ERR_INVALID_STATE;
  if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
    ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
  }
  if (ble_gap_adv_active()) ble_gap_adv_stop();
  nimble_port_stop();
  s_started = false;
  s_conn = BLE_HS_CONN_HANDLE_NONE;
  // DISCONNECT イベントが来ないケースに備えて lock も強制的に解放する。
  conn_lock_sync();
  bulk_lock_sync();
  return ESP_OK;
}

extern "C" bool ble_link_is_connected(void) {
  return s_conn != BLE_HS_CONN_HANDLE_NONE;
}

extern "C" esp_err_t ble_link_send_event(const uint8_t* cbor, size_t len) {
  if (!s_started || !cbor || len == 0) return ESP_ERR_INVALID_ARG;
  if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_event_notify) {
    return ESP_ERR_INVALID_STATE;
  }
  const bool ok = watch::proto::send_message(
      watch::proto::FrameType::Evt, s_evt_msg_id++, cbor, len, s_mtu,
      notify_emit, reinterpret_cast<void*>(s_event_val_handle));
  return ok ? ESP_OK : ESP_FAIL;
}

extern "C" esp_err_t ble_link_confirm_passkey(bool accept) {
  if (s_passkey_conn == BLE_HS_CONN_HANDLE_NONE) {
    return ESP_ERR_INVALID_STATE;
  }
  ble_sm_io io = {};
  io.action = BLE_SM_IOACT_NUMCMP;
  io.numcmp_accept = accept ? 1 : 0;
  const uint16_t conn = s_passkey_conn;
  s_passkey_conn = BLE_HS_CONN_HANDLE_NONE;
  return ble_sm_inject_io(conn, &io) == 0 ? ESP_OK : ESP_FAIL;
}

extern "C" bool ble_link_bulk_ready(void) {
  return s_conn != BLE_HS_CONN_HANDLE_NONE && s_bulk_notify;
}

extern "C" esp_err_t ble_link_bulk_send(uint8_t frame_type, uint16_t msg_id,
                                        const uint8_t* payload, size_t len) {
  if (!s_started || !payload || len == 0) return ESP_ERR_INVALID_ARG;
  if (!ble_link_bulk_ready()) return ESP_ERR_INVALID_STATE;
  (void)s_bulk_tx_msg_id;  // 送信側でも連番を持つ (いまは msg_id を使い回す)
  const bool ok = watch::proto::send_message(
      static_cast<watch::proto::FrameType>(frame_type), msg_id, payload, len,
      s_mtu, notify_emit, reinterpret_cast<void*>(s_bulk_val_handle));
  return ok ? ESP_OK : ESP_FAIL;
}
