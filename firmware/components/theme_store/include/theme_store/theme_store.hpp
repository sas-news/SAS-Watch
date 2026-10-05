// theme_store.hpp — /assets littlefs 上のテーマ資産管理。
//   docs/theme-format.md: BLE BULK (kind="theme") で受けた stored zip を
//   sha256 検証後にストリーミング展開し、適用待ち id を登録する。
//   適用自体は app タスクが pending を拾って SetTheme Action を投げる。
#pragma once

#include <cstddef>
#include <cstdint>

namespace theme_store {

// /assets (partitions.csv の assets パーティション) を littlefs で
// マウントし、themes/ ディレクトリを用意する。app_main から1度呼ぶ。
bool init();

// BLE BULK 受信用 Sink (watch::BulkReceiver::Sink と同じ形)。
// kind=="theme" のみ受理。NimBLE タスク上で呼ばれる。
bool bulk_begin(uint16_t id, const char* kind, uint32_t size, void* ctx);
bool bulk_write(uint16_t id, uint32_t offset, const uint8_t* data, size_t len,
                void* ctx);
bool bulk_commit(uint16_t id, void* ctx);
void bulk_abort(uint16_t id, void* ctx);

// commit で有効化待ちになった theme id を取り出す (無ければ false)。
// app タスク (ble_glue_poll) から呼ぶ。
bool take_pending_theme(char* out, size_t cap);

// settings.set {theme:...} で来た変更を適用待ちに登録する。
// dispatch の setting_changed コールバックから呼ぶ (NimBLE タスク)。
void set_pending_theme(const char* id);

}  // namespace theme_store
