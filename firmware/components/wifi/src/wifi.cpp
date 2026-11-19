// wifi — Wi-Fi セッション管理。
//   session_start: netif/イベントループ/drv を立ち上げ STA 接続 (IP まで待つ)。
//   session_end  : disconnect → stop → deinit → netif 破棄 → ループ削除。
//   資格情報は KeyValueStore (NVS) の "wifi.ssid" / "wifi.pass" に入れる。
#include "wifi/wifi.hpp"

#include <atomic>
#include <cstring>

#include "ble_link.h"
#include "esp_coexist.h"
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
constexpr EventBits_t kBitDisconnected = 1u << 2;
constexpr uint8_t kMaxRetries = 8;
// AP の接続直後バンドステアリング等の瞬間切断 (reason=2、最大 ~2.85s 観測) を
// session_start() 内で吸収する安定待ち。
constexpr uint32_t kConnectSettleMs = 4000;

constexpr uint32_t kBackoffMs[] = {3000, 10000, 22000, 35000, 40000};

uint32_t retry_backoff_ms(uint8_t retry_one_based) {
  constexpr size_t kCount = sizeof(kBackoffMs) / sizeof(kBackoffMs[0]);
  if (retry_one_based == 0) return kBackoffMs[0];
  const size_t idx = static_cast<size_t>(retry_one_based - 1);
  return kBackoffMs[idx < kCount ? idx : (kCount - 1)];
}

watch::KeyValueStore* s_kv = nullptr;
EventGroupHandle_t s_events = nullptr;
esp_netif_t* s_netif = nullptr;
esp_event_handler_instance_t s_wifi_h = nullptr;
esp_event_handler_instance_t s_ip_h = nullptr;
uint8_t s_retries = 0;
volatile bool s_connected = false;
volatile bool s_reconnecting = false;
volatile uint8_t s_last_reason = 0;
volatile bool s_in_connect_loop = false;
bool s_driver_up = false;
wifi_config_t s_active_conf = {};
wifi_auth_mode_t s_target_auth = WIFI_AUTH_WPA2_PSK;
std::atomic<bool> s_need_driver_down{false};
std::atomic<bool> s_need_cycle_reconnect{false};
std::atomic<TickType_t> s_reconnect_due_tick{0};
std::atomic<bool> s_cycling{false};
bool s_own_loop = false;  // session_start で作ったループは end で消す

void on_event(void*, esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    // 自動接続せず session_start() / poll() から esp_wifi_set_ps(WIFI_PS_NONE) の
    // 後に明示的に esp_wifi_connect() する。
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    const auto* ev = static_cast<const wifi_event_sta_disconnected_t*>(data);
    const uint8_t reason = ev ? ev->reason : 0u;
    s_last_reason = reason;
    s_connected = false;
    xEventGroupClearBits(s_events, kBitConnected);
    if (s_cycling.load() || s_netif == nullptr) {
      return;
    }
    ESP_LOGW(TAG, "disconnected reason=%u rssi=%d retry=%u",
             reason, ev ? ev->rssi : 0, static_cast<unsigned>(s_retries));
    if (s_in_connect_loop) {
      s_reconnecting = true;
      xEventGroupSetBits(s_events, kBitDisconnected);
    } else if (s_retries++ < kMaxRetries) {
      s_reconnecting = true;
      const uint32_t backoff_ms = retry_backoff_ms(s_retries);
      s_reconnect_due_tick.store(xTaskGetTickCount() + pdMS_TO_TICKS(backoff_ms));
      s_need_driver_down.store(true);
      s_need_cycle_reconnect.store(true);
    } else {
      s_reconnecting = false;
      s_need_driver_down.store(false);
      s_need_cycle_reconnect.store(false);
      xEventGroupSetBits(s_events, kBitFailed);
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    s_retries = 0;
    s_reconnecting = false;
    s_need_driver_down.store(false);
    s_need_cycle_reconnect.store(false);
    s_connected = true;
    xEventGroupSetBits(s_events, kBitConnected);
  }
}

void driver_down() {
  if (!s_driver_up && !s_wifi_h && !s_ip_h) return;
  s_cycling.store(true);
  s_need_driver_down.store(false);
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
  s_driver_up = false;
  s_cycling.store(false);
}

bool driver_up_common() {
  if (!s_netif) return false;
  wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
  if (esp_wifi_init(&init_cfg) != ESP_OK) return false;
  s_driver_up = true;
  if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event,
                                          nullptr, &s_wifi_h) != ESP_OK ||
      esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                          on_event, nullptr, &s_ip_h) !=
          ESP_OK ||
      esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
      esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) {
    driver_down();
    return false;
  }
  wifi_country_t country = {};
  std::memcpy(country.cc, "US", 2);
  country.schan = 1;
  country.nchan = 13;
  country.policy = WIFI_COUNTRY_POLICY_MANUAL;
  (void)esp_wifi_set_country(&country);
  return true;
}

bool driver_up(bool disable_pmf) {
  if (!driver_up_common()) return false;
  s_active_conf.sta.pmf_cfg.capable = !disable_pmf;
  s_active_conf.sta.pmf_cfg.required = false;
  if (esp_wifi_set_config(WIFI_IF_STA, &s_active_conf) != ESP_OK) {
    driver_down();
    return false;
  }
  if (disable_pmf) {
    // IDF 5.5 では pmf_cfg.capable=false が無視されるため、set_config 直後・
    // start 直前に esp_wifi_disable_pmf_config() で明示的に PMF を外す。
    (void)esp_wifi_disable_pmf_config(WIFI_IF_STA);
  }
  if (esp_wifi_start() != ESP_OK) {
    driver_down();
    return false;
  }
  (void)esp_wifi_set_ps(WIFI_PS_NONE);
  return true;
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
  ESP_LOGI(TAG, "credentials saved (ssid_len=%u, pass_len=%u)",
           static_cast<unsigned>(std::strlen(ssid)),
           static_cast<unsigned>(std::strlen(pass)));
  return true;
}

bool ssid(char* out, size_t cap) {
  return out && cap > 0 && read_cred(out, cap, kKeySsid);
}

bool configured() {
  char tmp[40];
  return ssid(tmp, sizeof(tmp));
}

void session_cleanup() {
  s_in_connect_loop = false;
  s_reconnecting = false;
  s_connected = false;
  s_need_driver_down.store(false);
  s_need_cycle_reconnect.store(false);
  driver_down();
  std::memset(&s_active_conf, 0, sizeof(s_active_conf));
  infra_down(s_own_loop);
  s_own_loop = false;
  (void)esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
  ble_link_set_wifi_quiet(false);
}

bool start_retry_attempt(bool set_bits_on_fail) {
  if (s_retries >= 4 && s_active_conf.sta.bssid_set) {
    s_active_conf.sta.bssid_set = false;
    s_active_conf.sta.channel = 0;
    s_active_conf.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    ESP_LOGI(TAG, "falling back to all-channel scan for attempt %u",
             static_cast<unsigned>(s_retries));
  }
  const bool disable_pmf =
      (s_target_auth != WIFI_AUTH_WPA3_PSK) && ((s_retries % 2) == 0);
  xEventGroupClearBits(s_events,
                       kBitConnected | kBitDisconnected | kBitFailed);
  ESP_LOGI(TAG, "attempt %u: disable_pmf=%d",
           static_cast<unsigned>(s_retries), disable_pmf ? 1 : 0);
  if (!driver_up(disable_pmf)) {
    s_reconnecting = false;
    if (set_bits_on_fail) xEventGroupSetBits(s_events, kBitFailed);
    return false;
  }
  s_reconnecting = true;
  if (esp_wifi_connect() != ESP_OK) {
    driver_down();
    s_reconnecting = false;
    if (set_bits_on_fail) xEventGroupSetBits(s_events, kBitFailed);
    return false;
  }
  return true;
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

  ble_link_set_wifi_quiet(true);
  (void)esp_coex_preference_set(ESP_COEX_PREFER_WIFI);

  s_own_loop = false;
  if (!infra_up(&s_own_loop)) {
    (void)esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
    ble_link_set_wifi_quiet(false);
    return false;
  }

  std::memset(&s_active_conf, 0, sizeof(s_active_conf));
  std::memcpy(s_active_conf.sta.ssid, ssid,
              strnlen(ssid, sizeof(s_active_conf.sta.ssid)));
  std::memcpy(s_active_conf.sta.password, pass,
              strnlen(pass, sizeof(s_active_conf.sta.password)));
  s_active_conf.sta.threshold.authmode = WIFI_AUTH_OPEN;
  s_active_conf.sta.pmf_cfg.capable = true;
  s_active_conf.sta.pmf_cfg.required = false;
  s_active_conf.sta.listen_interval = 10;
  s_active_conf.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  s_active_conf.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

  // 対象 SSID だけスキャンして BSSID/ch/authmode を特定し、以後のリトライで
  // 13ch プローブラッシュを防ぐ (WIFI_FAST_SCAN + 固定 BSSID/ch)。
  // スキャン後は完全に driver_down() してから Attempt 0 を起こす。
  s_target_auth = WIFI_AUTH_WPA2_PSK;
  s_in_connect_loop = false;
  if (!driver_up_common()) {
    session_cleanup();
    return false;
  }
  s_cycling.store(true);
  if (esp_wifi_start() == ESP_OK) {
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    wifi_scan_config_t sc = {};
    sc.ssid = reinterpret_cast<uint8_t*>(ssid);
    sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
      uint16_t ap_num = 0;
      (void)esp_wifi_scan_get_ap_num(&ap_num);
      if (ap_num > 0) {
        wifi_ap_record_t recs[8] = {};
        uint16_t want = ap_num > 8 ? 8 : ap_num;
        if (esp_wifi_scan_get_ap_records(&want, recs) == ESP_OK && want > 0) {
          s_target_auth = recs[0].authmode;
          std::memcpy(s_active_conf.sta.bssid, recs[0].bssid, 6);
          s_active_conf.sta.bssid_set = true;
          s_active_conf.sta.channel = recs[0].primary;
          s_active_conf.sta.scan_method = WIFI_FAST_SCAN;
          ESP_LOGI(TAG,
                   "scan pinned bssid=%02x:%02x:%02x:%02x:%02x:%02x ch=%u rssi=%d auth=%d",
                   recs[0].bssid[0], recs[0].bssid[1], recs[0].bssid[2],
                   recs[0].bssid[3], recs[0].bssid[4], recs[0].bssid[5],
                   static_cast<unsigned>(recs[0].primary),
                   static_cast<int>(recs[0].rssi),
                   static_cast<int>(recs[0].authmode));
        }
      } else {
        ESP_LOGW(TAG, "targeted scan found 0 APs; using all-channel scan");
      }
      (void)esp_wifi_clear_ap_list();
    }
  }
  driver_down();

  s_retries = 0;
  s_last_reason = 0;
  s_need_driver_down.store(false);
  s_need_cycle_reconnect.store(false);
  s_in_connect_loop = true;
  if (!start_retry_attempt(false)) {
    session_cleanup();
    return false;
  }

  {
    const TickType_t start_tick = xTaskGetTickCount();
    const TickType_t total_ticks = pdMS_TO_TICKS(timeout_ms);
    for (;;) {
      const TickType_t elapsed = xTaskGetTickCount() - start_tick;
      if (elapsed >= total_ticks) {
        ESP_LOGW(TAG, "connect timeout");
        break;
      }
      const EventBits_t bits = xEventGroupWaitBits(
          s_events, kBitConnected | kBitDisconnected | kBitFailed,
          pdFALSE, pdFALSE, total_ticks - elapsed);
      if (bits & kBitFailed) {
        ESP_LOGW(TAG, "connect failed");
        break;
      }
      if (bits & kBitConnected) {
        vTaskDelay(pdMS_TO_TICKS(kConnectSettleMs));
        if (s_connected) {
          s_in_connect_loop = false;
          ESP_LOGI(TAG, "connected (ssid_len=%u)",
                   static_cast<unsigned>(std::strlen(ssid)));
          return true;
        }
        ESP_LOGI(TAG, "transient disconnect during settle; waiting for reconnect");
        continue;
      }
      if (bits & kBitDisconnected) {
        xEventGroupClearBits(s_events, kBitDisconnected);
        // バックオフ中は deinit まで落として完全に RF を止める。
        driver_down();
        if (s_retries++ >= kMaxRetries) {
          s_reconnecting = false;
          ESP_LOGW(TAG, "connect retries exhausted");
          break;
        }
        const TickType_t used = xTaskGetTickCount() - start_tick;
        if (used >= total_ticks) {
          ESP_LOGW(TAG, "connect timeout");
          break;
        }
        const uint32_t rem_ms =
            static_cast<uint32_t>((total_ticks - used) * portTICK_PERIOD_MS);
        if (rem_ms <= 2500u) {
          ESP_LOGW(TAG, "connect timeout");
          break;
        }
        uint32_t backoff_ms = retry_backoff_ms(s_retries);
        if (backoff_ms + 2500u > rem_ms) {
          backoff_ms = rem_ms - 2500u;
        }
        ESP_LOGI(TAG, "rf-off backoff %u ms before attempt %u",
                 static_cast<unsigned>(backoff_ms),
                 static_cast<unsigned>(s_retries));
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        if (!s_connected && !start_retry_attempt(false)) {
          ESP_LOGW(TAG, "start_retry_attempt failed");
          break;
        }
      }
    }
  }

  session_cleanup();
  return false;
}

void session_end() {
  if (s_netif == nullptr && !s_connected) return;
  session_cleanup();
  ESP_LOGI(TAG, "session end (wifi off)");
}

bool connected() { return s_connected; }
bool reconnecting() { return s_reconnecting; }

void poll() {
  if (s_in_connect_loop || s_netif == nullptr) {
    return;
  }
  if (s_need_driver_down.exchange(false)) {
    driver_down();
  }
  if (!s_need_cycle_reconnect.load()) {
    return;
  }
  const TickType_t now = xTaskGetTickCount();
  const TickType_t due = s_reconnect_due_tick.load();
  if (static_cast<int32_t>(now - due) < 0) return;
  s_need_cycle_reconnect.store(false);
  if (s_connected) {
    return;
  }
  ESP_LOGI(TAG, "session idle retry: restarting Wi-Fi driver");
  (void)start_retry_attempt(true);
}

}  // namespace wifi
