// ble_link — Phone(Android) との BLE リンク (NimBLE GATT サーバ)。
// 仕様は docs/protocol-v1.md。C API なので app_main (C++ 以外) からも呼べる。
//
// 使い方:
//   ble_link_config_t cfg = {0};
//   cfg.dispatch_req = ...;          // core/protocol の dispatcher を呼ぶ関数
//   cfg.on_conn_state = ...;         // 接続/切断の通知 (UI 更新用)
//   cfg.on_passkey    = ...;         // 6桁表示要求 → 画面に出す
//   ble_link_start(&cfg);
//   // パスキー表示中に UI の「はい/いいえ」→ ble_link_confirm_passkey(accept)
//   // EVT を送るとき → ble_link_send_event(cbor, len)
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// REQ ディスパッチ。再組み立て済みの REQ payload (CBOR {"m":..,"p":..}) を
// 受け取り、res_buf に RES payload (CBOR {"ok":..}) を書いて長さを返す。
// 0 を返すと呼び出し側は "busy" エラーの RES を組み立てて返す。
typedef size_t (*ble_link_dispatch_fn)(const uint8_t* req, size_t req_len,
                                       uint8_t* res_buf, size_t res_cap,
                                       void* ctx);

// 接続状態の通知。authenticated=true は「ボンド済みで暗号化済みのリンク」。
typedef void (*ble_link_conn_state_fn)(bool connected, bool authenticated,
                                       void* ctx);

// LE Secure Connections の Numeric Comparison: 6桁を画面に出してほしい。
// UI はユーザーの「はい/いいえ」を ble_link_confirm_passkey() に渡す。
typedef void (*ble_link_passkey_fn)(uint32_t passkey, void* ctx);

typedef struct {
  // 必須: REQ を core/protocol の dispatcher に渡す関数。
  // NULL の場合、届いた REQ はすべて "busy" エラーになる。
  ble_link_dispatch_fn dispatch_req;
  void* dispatch_ctx;

  // 任意: 状態通知コールバック (NULL 可)。
  ble_link_conn_state_fn on_conn_state;
  // 任意: パスキー表示 (NULL なら Numeric Comparison は自動で「いいえ」)。
  ble_link_passkey_fn on_passkey;
  void* cb_ctx;

  // BULK 転送の保存先 (protocol-v1.md BULK_*)。
  // 全部 NULL でもよい: その場合「受信して SHA-256 検証だけする」挙動。
  bool (*bulk_begin)(uint16_t id, const char* kind, uint32_t size, void* ctx);
  bool (*bulk_write)(uint16_t id, uint32_t offset, const uint8_t* data,
                     size_t len, void* ctx);
  // sha256 検証 OK 後に呼ぶ (確定処理: リネームして有効化など)。
  bool (*bulk_commit)(uint16_t id, void* ctx);
  // 転送中断/失敗時の掃除 (NULL 可)。
  void (*bulk_abort)(uint16_t id, void* ctx);
  void* bulk_ctx;

  // 時計→スマホ方向の BULK で、Phone 側の BULK_ACK {id,next} が届いたとき。
  // NimBLE タスクから呼ばれるので軽い処理だけにすること。
  void (*on_bulk_ack)(uint16_t id, uint32_t next, void* ctx);
} ble_link_config_t;

// NimBLE を起動して advertising を始める。2回目以降は ESP_ERR_INVALID_STATE。
esp_err_t ble_link_start(const ble_link_config_t* cfg);

// 停止 (切断して advertising も止める)。未起動なら ESP_ERR_INVALID_STATE。
esp_err_t ble_link_stop(void);

// BLE リンクが張っているか (ボンドの有無は問わない)。
bool ble_link_is_connected(void);

// EVT payload (CBOR {"e":..,"d":{..}}) を event characteristic で notify。
// 未接続/未購読なら ESP_ERR_INVALID_STATE。大きい EVT は MTU で分割される。
esp_err_t ble_link_send_event(const uint8_t* cbor, size_t len);

// on_passkey で表示した確認に UI から答える。表示中でなければ
// ESP_ERR_INVALID_STATE。
esp_err_t ble_link_confirm_passkey(bool accept);

// ---- 時計→スマホ方向の BULK 送信 (音声メモなど) ----
// bulk characteristic が notify 購読済みなら true。
bool ble_link_bulk_ready(void);
// 1 つの BULK_* メッセージ (payload = CBOR または CHUNK 生バイト) を
// bulk characteristic で notify する。frame_type は protocol の
// 0x10 BULK_START / 0x11 BULK_CHUNK / 0x13 BULK_END (ACK は時計側は送らない)。
// 未接続/未購読なら ESP_ERR_INVALID_STATE。
esp_err_t ble_link_bulk_send(uint8_t frame_type, uint16_t msg_id,
                             const uint8_t* payload, size_t len);

#ifdef __cplusplus
}
#endif
