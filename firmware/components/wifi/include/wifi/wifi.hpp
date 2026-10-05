// wifi — Wi-Fi セッション管理 + NVS の資格情報。
//   plan.md: 「時計単体の Wi-Fi は電池を食う。Wi-Fi は同期や OTA のときだけ」。
//   常時接続はしない。session_start でドライバごと立ち上げて接続し、
//   session_end で完全に落とす (netif/イベントループ/常駐タスクなし)。
//   資格情報は settings keys に入れず NVS キー "wifi.ssid" / "wifi.pass" に
//   直接保存するので pass の読み出し経路は存在しない。
#pragma once

#include <cstddef>

#include "watch/platform.hpp"

namespace wifi {

// 資格情報の置き場として kv を受け取る。Wi-Fi ドライバはまだ上げない。
void init(watch::KeyValueStore* kv);

// REQ wifi.set から。ssid (1-32) / pass (0 または 8-63) の検査は呼び出し側済み。
bool set_credentials(const char* ssid, const char* pass);

// 設定済みなら out に SSID を書いて true。pass はいかなる形でも返さない。
bool ssid(char* out, size_t cap);
bool configured();

// 保存済み資格情報で STA 接続 (IP 取得まで最大 timeout_ms)。
// 失敗しても Wi-Fi は落とした状態で返る。成功したら session_end で完全OFFに。
bool session_start(uint32_t timeout_ms);
void session_end();
bool connected();

}  // namespace wifi
