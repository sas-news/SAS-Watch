#include "platform_esp/platform_esp.hpp"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

// NVS blob の KeyValueStore。キーは "watch" namespace の直下。
// NVS のキー長制限 (15文字) は core 側のキー設計 ("feat.*", settings keys) が
// 既に満たしている。

namespace platform_esp {

static const char* TAG = "platform_esp.kv";
static constexpr const char* kNs = "watch";

static nvs_handle_t s_nvs = 0;
static bool s_ready = false;

bool NvsKv::init() {
  const esp_err_t ret = nvs_open(kNs, NVS_READWRITE, &s_nvs);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(ret));
    return false;
  }
  s_ready = true;
  return true;
}

size_t NvsKv::size(const char* key) {
  if (!s_ready || !key) return 0;
  size_t n = 0;
  if (nvs_get_blob(s_nvs, key, nullptr, &n) != ESP_OK) return 0;
  return n;
}

bool NvsKv::get(const char* key, void* buf, size_t buf_cap, size_t* out_len) {
  if (out_len) *out_len = 0;
  if (!s_ready || !key) return false;
  size_t n = 0;
  esp_err_t ret = nvs_get_blob(s_nvs, key, nullptr, &n);
  if (ret == ESP_ERR_NVS_NOT_FOUND) return false;
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "get %s: %s", key, esp_err_to_name(ret));
    return false;
  }
  if (n > buf_cap) {
    // 必要量を報告して false — 呼び出し側が大きいバッファで読み直せる。
    if (out_len) *out_len = n;
    return false;
  }
  ret = nvs_get_blob(s_nvs, key, buf, &n);
  if (ret != ESP_OK) return false;
  if (out_len) *out_len = n;
  return true;
}

bool NvsKv::set(const char* key, const void* data, size_t len) {
  if (!s_ready || !key) return false;
  esp_err_t ret;
  if (len == 0) {
    // len==0 は消去と同等 (platform.hpp の契約)。
    ret = nvs_erase_key(s_nvs, key);
    if (ret == ESP_ERR_NVS_NOT_FOUND) ret = ESP_OK;
  } else {
    ret = nvs_set_blob(s_nvs, key, data, len);
  }
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "set %s: %s", key, esp_err_to_name(ret));
    return false;
  }
  ret = nvs_commit(s_nvs);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "commit %s: %s", key, esp_err_to_name(ret));
    return false;
  }
  return true;
}

bool NvsKv::erase(const char* key) {
  if (!s_ready || !key) return false;
  const esp_err_t ret = nvs_erase_key(s_nvs, key);
  if (ret == ESP_ERR_NVS_NOT_FOUND) return true;  // 元から無い = 消えている
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "erase %s: %s", key, esp_err_to_name(ret));
    return false;
  }
  return nvs_commit(s_nvs) == ESP_OK;
}

}  // namespace platform_esp
