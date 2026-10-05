// ota — OTA ファーム更新セッション。
//   経路は2つ。どちらも「必要なときだけ」動いて終わったら元に戻す。
//     - HTTPS: ota.start {url,sha256,version} → Wi-Fi セッション →
//              esp_https_ota → sha256 照合 → set_boot → 再起動
//     - BLE  : BULK kind="firmware" を /assets/.fw.bulk に受け →
//              commit 後に esp_ota_* で書き込み → set_boot → 再起動
//              (パーティション消去は重いので NimBLE タスク上ではやらない)
//   進捗は status() のスナップショットとして共有し、app タスクの poll() が
//   EventBus の OtaProgress と電源 Lease (OTA 中の deep sleep 禁止) を司る。
#pragma once

#include <cstddef>
#include <cstdint>

#include "watch/event_bus.hpp"
#include "watch/power.hpp"

namespace ota {

// protocol-v1.md の ota.status stage と対応 (stage_name() が文字列化)。
enum class Stage : uint8_t {
  Idle = 0,
  Wifi,      // Wi-Fi 接続中
  Download,  // イメージ受信中 (HTTPS or BLE)
  Verify,    // イメージ検証・パーティション書き込み中
  Done,      // 完了 (直後に再起動)
  Reboot,
  Fail,
};

const char* stage_name(Stage s);

struct Status {
  Stage stage = Stage::Idle;
  uint8_t pct = 0;        // 0-100
  char msg[40] = {};      // 失敗理由 / 補足 (短い ASCII)
  char version[24] = {};  // 更新先の表示名
};

// power は Lease 用、bus は進捗イベント用。ワーカータスクを作る。
void init(watch::PowerPolicy* power, watch::EventBus* bus);
Status status();
// 進行中か (Idle/Fail 以外 = 新しい start/bulk を受け付けない)。
bool busy();

// dispatch ota.start から呼ぶ (速く返る必要あり)。0=開始 / 1=busy / -1=失敗。
int start_https(const char* url, const uint8_t sha256[32], const char* version);

// BLE BULK (kind="firmware") の sink。bulk_begin が false = BULK 拒否。
bool bulk_begin(uint16_t id, uint32_t size);
bool bulk_write(uint16_t id, uint32_t offset, const uint8_t* data, size_t len);
bool bulk_commit(uint16_t id);
void bulk_abort(uint16_t id);

// app タスクから毎ループ: 進捗イベント化 + Lease の取得/返却。
void poll();

// app_main の自己診断 (初期化全部成功) 後に1回呼ぶ。
// CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 時、新イメージの初回起動なら
// mark valid して次回リセットでのロールバックを防ぐ。
void mark_valid_if_pending();

}  // namespace ota
