// wifi — Wi-Fi セッション管理。
//   session_start: netif/イベントループ/drv を立ち上げ STA 接続 (IP まで待つ)。
//   session_end  : disconnect → stop → deinit → netif 破棄 → ループ削除。
//   資格情報は KeyValueStore (NVS) の "wifi.ssid" / "wifi.pass" に入れる。
#include "wifi/wifi.hpp"

#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

namespace wifi {

namespace {

constexpr const char* TAG = "wifi";
// NVS キー (15文字以内。settings keys の "set.*" 系とは別系統)。
constexpr const char* kKeySsid = "wifi.ssid";
constexpr const char* kKeyPass = "wifi.pass";

constexpr EventBits_t kBitConnected = 1u << 0;
constexpr EventBits_t kBitFailed = 1u << 1;
constexpr uint8_t kMaxRetries = 3;
// 接続が安定するまでの待機。TODO(hw): 実機の AP 応答で調整
constexpr uint32_t kConnectDelayMs = 50;

watch::KeyValueStore* s_kv = nullptr;
EventGroupHandle_t s_events = nullptr;
esp_netif_t* s_netif = nullptr;
esp_event_handler_instance_t s_wifi_h = nullptr;
esp_event_handler_instance_t s_ip_h = nullptr;
uint8_t s_retries = 0;
bool s_connected = false;
bool s_own_loop = false;  // session_start で作ったループは end で消す

void on_event(void*, esp_event_base_t base, int32_t id, void*) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    s_connected = false;
    if (s_retries++ < kMaxRetries) {
      esp_wifi_connect();
    } else {
      xEventGroupSetBits(s_events, kBitFailed);
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    s_connected = true;
    xEventGroupSetBits(s_events, kBitConnected);
  }
}

bool read_cred(char* out, size_t cap, const char* key) {
  if (!s_kv) return false;
  const size_t n = s_kv->size(key);
  if (n == 0 || n > cap) return false;
  size_t got = 0;
  if (!s_kv->get(key, out, cap, &got) || got == 0) return false;
  if (out[got - 1] != '\0') {
    if (got >= cap) return false;
    out[got] = '\0';
  }
  return out[0] != '\0';
}

// イベントループと STA netif の用意 (esp_netif_init は1回だけ)。
bool infra_up(bool* own_loop) {
  *own_loop = false;
  esp_err_t err = esp_netif_init();
  if (err != ESP_OK) return false;
  err = esp_event_loop_create_default();
  if (err == ESP_ERR_INVALID_STATE) {
    // 既に誰かが作っている → それを共有し、終了時は壊さない。
    err = ESP_OK;
  } else if (err == ESP_OK) {
    *own_loop = true;
  }
  if (err != ESP_OK) return false;
  s_netif = esp_netif_create_default_wifi_sta();
  if (!s_netif) {
    if (*own_loop) esp_event_loop_delete_default();
    return false;
  }
  return true;
}

void infra_down(bool own_loop) {
  if (s_netif) {
    esp_netif_destroy_default_wifi(s_netif);
    s_netif = nullptr;
  }
  if (own_loop && esp_event_loop_delete_default() != ESP_OK) {
    // 削除に失敗してもループが残るだけ (Wi-Fi ドライバ自体は落ちている)。
    ESP_LOGW(TAG, "event loop delete failed");
  }
}

}  // namespace

void init(watch::KeyValueStore* kv) {
  s_kv = kv;
  if (!s_events) s_events = xEventGroupCreate();
}

bool set_credentials(const char* ssid, const char* pass) {
  if (!s_kv || !ssid || !pass || ssid[0] == '\0') return false;
  if (!s_kv->set(kKeySsid, ssid, std::strlen(ssid) + 1)) return false;
  if (!s_kv->set(kKeyPass, pass, std::strlen(pass) + 1)) return false;
  ESP_LOGI(TAG, "credentials saved ssid=%s", ssid);
  return true;
}

bool ssid(char* out, size_t cap) {
  return out && cap > 0 && read_cred(out, cap, kKeySsid);
}

bool configured() {
  char tmp[40];
  return ssid(tmp, sizeof(tmp));
}

bool session_start(uint32_t timeout_ms) {
  if (!s_events) return false;
  if (s_connected) return true;

  char ssid[33], pass[64];
  if (!read_cred(ssid, sizeof(ssid), kKeySsid)) {
    ESP_LOGW(TAG, "no credentials");
    return false;
  }
  if (!read_cred(pass, sizeof(pass), kKeyPass)) pass[0] = '\0';

  s_own_loop = false;
  if (!infra_up(&s_own_loop)) return false;

  wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
  if (esp_wifi_init(&init_cfg) != ESP_OK) {
    infra_down(s_own_loop);
    s_own_loop = false;
    return false;
  }
  esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event,
                                      nullptr, &s_wifi_h);
  esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event,
                                      nullptr, &s_ip_h);
  if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
      esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) {
    goto fail;
  }
  {
    wifi_config_t conf = {};
    std::strncpy(reinterpret_cast<char*>(conf.sta.ssid), ssid,
                 sizeof(conf.sta.ssid));
    std::strncpy(reinterpret_cast<char*>(conf.sta.password), pass,
                 sizeof(conf.sta.password));
    conf.sta.threshold.authmode =
        pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    conf.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    conf.sta.pmf_cfg.capable = true;  // WPA2/WPA3 移行 AP 用 // TODO(hw): 実機で確認
    conf.sta.pmf_cfg.required = false;
    if (esp_wifi_set_config(WIFI_IF_STA, &conf) != ESP_OK) goto fail;
  }
  s_retries = 0;
  xEventGroupClearBits(s_events, kBitConnected | kBitFailed);
  if (esp_wifi_start() != ESP_OK) goto fail;

  {
    const EventBits_t bits = xEventGroupWaitBits(
        s_events, kBitConnected | kBitFailed, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));
    if (bits & kBitConnected) {
      vTaskDelay(pdMS_TO_TICKS(kConnectDelayMs));
      ESP_LOGI(TAG, "connected ssid=%s", ssid);
      return true;
    }
    ESP_LOGW(TAG, "connect %s", (bits & kBitFailed) ? "failed" : "timeout");
  }
  esp_wifi_disconnect();

fail:
  esp_wifi_stop();
  esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_h);
  esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_h);
  s_wifi_h = s_ip_h = nullptr;
  esp_wifi_deinit();
  infra_down(s_own_loop);
  s_own_loop = false;
  return false;
}

void session_end() {
  if (s_netif == nullptr && !s_connected) return;
  s_connected = false;
  esp_wifi_disconnect();
  esp_wifi_stop();
  if (s_wifi_h) {
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                          s_wifi_h);
    s_wifi_h = nullptr;
  }
  if (s_ip_h) {
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                          s_ip_h);
    s_ip_h = nullptr;
  }
  esp_wifi_deinit();
  infra_down(true);  // 自分が立てたイベントループなら削除して常駐を残さない
  ESP_LOGI(TAG, "session end (wifi off)");
}

bool connected() { return s_connected; }

}  // namespace wifi
