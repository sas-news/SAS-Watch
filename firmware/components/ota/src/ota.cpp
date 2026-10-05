// ota — OTA セッション実装。
//   状態: s_st を seqlock 風に共有 (書き手は ota タスク or NimBLE タスク、
//   読み手は app/LVGL/dispatch)。ジョブは通知でワーカータスクへ渡す。
#include "ota/ota.hpp"

#include <cstdio>
#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "watch/event.hpp"
#include "watch/protocol/sha256.hpp"
#include "wifi/wifi.hpp"

namespace ota {

namespace {

constexpr const char* TAG = "ota";
constexpr const char* kBulkFile = "/assets/.fw.bulk";
// 完了 EVT が Phone に届くまでの猶予。
constexpr uint32_t kRebootDelayMs = 1800;
// HTTPS 開始前の Wi-Fi 接続待ち。
constexpr uint32_t kWifiTimeoutMs = 25000;
constexpr size_t kIoBuf = 4096;

watch::PowerPolicy* s_power = nullptr;
watch::EventBus* s_bus = nullptr;
TaskHandle_t s_task = nullptr;

// ---- status の共有 (seqlock-lite) -----------------------------------------
volatile uint32_t s_seq = 0;
Status s_st;
bool s_ble = false;  // BLE 経路の進行中か (Lease の Res 選択用)

void set_status(Stage st, int pct, const char* msg) {
  ++s_seq;
  s_st.stage = st;
  if (pct >= 0) s_st.pct = pct > 100 ? 100 : static_cast<uint8_t>(pct);
  if (msg) {
    std::strncpy(s_st.msg, msg, sizeof(s_st.msg) - 1);
    s_st.msg[sizeof(s_st.msg) - 1] = '\0';
  }
  ++s_seq;
}

void set_version(const char* v) {
  ++s_seq;
  std::strncpy(s_st.version, v ? v : "", sizeof(s_st.version) - 1);
  s_st.version[sizeof(s_st.version) - 1] = '\0';
  ++s_seq;
}

// ---- ジョブ (producer: NimBLE タスク / worker: s_task) ----------------------
struct Job {
  int type = 0;  // 1=https, 2=file
  char url[256] = {};
  uint8_t sha[32] = {};
};
Job s_job;
volatile bool s_job_ready = false;

// BLE 受信ステージング
FILE* s_f = nullptr;
uint32_t s_fsize = 0;

watch::PowerPolicy::Lease s_lease;

// ---- ワーカーの処理本体 ------------------------------------------------------

// パーティションの先頭 len バイトを読み戻して sha256 と照合する。
// (esp_ota_write はファイルのバイトをそのまま書くので一致するはず)
bool verify_partition_sha(const esp_partition_t* part, uint32_t len,
                          const uint8_t expected[32]) {
  watch::proto::Sha256 h;
  uint8_t buf[kIoBuf];
  for (uint32_t off = 0; off < len;) {
    const uint32_t n =
        (len - off) < sizeof(buf) ? (len - off) : sizeof(buf);
    if (esp_partition_read(part, off, buf, n) != ESP_OK) return false;
    h.update(buf, n);
    off += n;
  }
  uint8_t sha[32];
  h.finish(sha);
  return std::memcmp(sha, expected, sizeof(sha)) == 0;
}

void finish_ok() {
  set_status(Stage::Done, 100, "reboot");
  ESP_LOGI(TAG, "update ok → reboot in %ums", kRebootDelayMs);
  vTaskDelay(pdMS_TO_TICKS(kRebootDelayMs));
  esp_restart();
}

// HTTPS 経路: Wi-Fi を上げて esp_https_ota で書き込む。
void https_run(const Job& j) {
  const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
  if (!part) {
    set_status(Stage::Fail, 0, "partition");
    return;
  }
  set_status(Stage::Wifi, 0, "");
  if (!wifi::session_start(kWifiTimeoutMs)) {
    set_status(Stage::Fail, 0, "wifi");
    return;
  }

  set_status(Stage::Download, 0, "");
  const esp_http_client_config_t http_cfg = {
      .url = j.url,
      .timeout_ms = 15000,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .keep_alive_enable = true,
  };
  const esp_https_ota_config_t ota_cfg = {
      .http_config = &http_cfg,
  };
  esp_https_ota_handle_t h = nullptr;
  const char* fail = nullptr;
  int img_len = 0;
  if (esp_https_ota_begin(&ota_cfg, &h) != ESP_OK) {
    fail = "begin";
  } else {
    const int total = esp_https_ota_get_image_size(h);
    for (;;) {
      const esp_err_t e = esp_https_ota_perform(h);
      if (e == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        const int rd = esp_https_ota_get_image_len_read(h);
        if (total > 0 && rd > 0) {
          set_status(Stage::Download, rd * 100 / total, "");
        }
        continue;
      }
      if (e != ESP_OK) fail = "download";
      break;
    }
    img_len = esp_https_ota_get_image_len_read(h);
    if (fail) {
      esp_https_ota_abort(h);
    } else if (esp_https_ota_finish(h) != ESP_OK) {
      fail = "image";
    }
  }
  wifi::session_end();

  if (fail) {
    set_status(Stage::Fail, 0, fail);
    return;
  }
  set_status(Stage::Verify, 95, "");
  if (!verify_partition_sha(part, static_cast<uint32_t>(img_len), j.sha)) {
    set_status(Stage::Fail, 0, "sha256");
    return;
  }
  if (esp_ota_set_boot_partition(part) != ESP_OK) {
    set_status(Stage::Fail, 0, "boot");
    return;
  }
  finish_ok();
}

// BLE 経路: /assets/.fw.bulk に受けたファイルをパーティションへ書き込む。
void file_run() {
  set_status(Stage::Verify, 0, "");
  const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
  FILE* f = std::fopen(kBulkFile, "rb");
  esp_ota_handle_t h = 0;
  const char* fail = nullptr;
  uint32_t written = 0;
  if (!part || !f) {
    fail = "file";
  } else if (esp_ota_begin(part, s_fsize, &h) != ESP_OK) {
    fail = "begin";
  } else {
    uint8_t buf[kIoBuf];
    for (;;) {
      const size_t n = std::fread(buf, 1, sizeof(buf), f);
      if (n == 0) break;
      if (esp_ota_write(h, buf, n) != ESP_OK) {
        fail = "write";
        break;
      }
      written += n;
      set_status(Stage::Verify,
                 s_fsize ? static_cast<int>(written * 100 / s_fsize) : 0, "");
    }
    if (!fail && esp_ota_end(h) != ESP_OK) fail = "image";
  }
  if (f) std::fclose(f);
  std::remove(kBulkFile);
  if (fail) {
    set_status(Stage::Fail, 0, fail);
    return;
  }
  if (esp_ota_set_boot_partition(part) != ESP_OK) {
    set_status(Stage::Fail, 0, "boot");
    return;
  }
  finish_ok();
}

void ota_task(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (!s_job_ready) continue;
    const Job j = s_job;
    s_job_ready = false;
    s_job = Job{};
    if (j.type == 1) {
      https_run(j);
    } else if (j.type == 2) {
      file_run();
    }
    s_ble = false;
  }
}

}  // namespace

const char* stage_name(Stage s) {
  switch (s) {
    case Stage::Idle: return "idle";
    case Stage::Wifi: return "wifi";
    case Stage::Download: return "download";
    case Stage::Verify: return "verify";
    case Stage::Done: return "done";
    case Stage::Reboot: return "reboot";
    case Stage::Fail: return "fail";
  }
  return "idle";
}

void init(watch::PowerPolicy* power, watch::EventBus* bus) {
  s_power = power;
  s_bus = bus;
  if (!s_task) {
    // セッション中しか走らない待機タスク (stack は初期化時のみ確保)。
    xTaskCreate(ota_task, "ota", 8192, nullptr, 4, &s_task);
  }
}

Status status() {
  for (;;) {
    const uint32_t s1 = s_seq;
    if (s1 & 1u) continue;  // 書き込み中 → 取り直し
    const Status copy = s_st;
    if (s_seq == s1) return copy;
  }
}

bool busy() {
  const Stage s = status().stage;
  return s != Stage::Idle && s != Stage::Fail;
}

int start_https(const char* url, const uint8_t sha256[32],
                const char* version) {
  if (!url || !sha256 || busy()) return 1;
  if (!wifi::configured()) {
    set_status(Stage::Fail, 0, "wifi未設定");
    return -1;
  }
  s_ble = false;
  s_job = Job{};
  s_job.type = 1;
  std::strncpy(s_job.url, url, sizeof(s_job.url) - 1);
  std::memcpy(s_job.sha, sha256, sizeof(s_job.sha));
  set_version(version);
  s_job_ready = true;
  set_status(Stage::Wifi, 0, "");
  ESP_LOGI(TAG, "ota start url=%s ver=%s", url, version);
  if (s_task) xTaskNotifyGive(s_task);
  return 0;
}

bool bulk_begin(uint16_t, uint32_t size) {
  if (busy()) return false;
  const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
  if (!part || size == 0 || size > part->size) {
    ESP_LOGW(TAG, "firmware size %lu rejected (part %lu)",
             static_cast<unsigned long>(size),
             part ? static_cast<unsigned long>(part->size) : 0);
    return false;
  }
  // ステージング先 (/assets littlefs) の空きを確認。
  size_t total = 0, used = 0;
  if (esp_littlefs_info("assets", &total, &used) != ESP_OK ||
      size > (total - used)) {
    ESP_LOGW(TAG, "no space for firmware (%lu > %lu)",
             static_cast<unsigned long>(size),
             static_cast<unsigned long>(total - used));
    return false;
  }
  if (s_f) {
    std::fclose(s_f);
    s_f = nullptr;
  }
  s_f = std::fopen(kBulkFile, "w+b");
  if (!s_f) return false;
  s_fsize = size;
  s_ble = true;
  set_version("BLE");
  set_status(Stage::Download, 0, "BLE");
  ESP_LOGI(TAG, "firmware bulk start size=%lu",
           static_cast<unsigned long>(size));
  return true;
}

bool bulk_write(uint16_t, uint32_t offset, const uint8_t* data, size_t len) {
  if (!s_f || !data || offset + len > s_fsize) return false;
  if (std::fseek(s_f, offset, SEEK_SET) != 0) return false;
  if (std::fwrite(data, 1, len, s_f) != len) return false;
  // 進捗は 1% 刻みでしか通知しない (イベント洪水を防ぐ)。
  const uint8_t pct = s_fsize ? static_cast<uint8_t>(offset * 100 / s_fsize) : 0;
  if (pct != s_st.pct) set_status(Stage::Download, pct, "BLE");
  return true;
}

bool bulk_commit(uint16_t) {
  if (s_f) {
    std::fclose(s_f);
    s_f = nullptr;
  }
  s_job = Job{};
  s_job.type = 2;
  s_job_ready = true;
  ESP_LOGI(TAG, "firmware bulk committed → flash");
  if (s_task) xTaskNotifyGive(s_task);
  return true;
}

void bulk_abort(uint16_t) {
  if (s_f) {
    std::fclose(s_f);
    s_f = nullptr;
  }
  std::remove(kBulkFile);
  set_status(Stage::Idle, 0, "");
}

void poll() {
  if (!s_power) return;
  const Status s = status();
  const bool active = s.stage != Stage::Idle && s.stage != Stage::Fail;
  // OTA 中は deep sleep 禁止 (plan.md L章の Lease)。
  if (active && !s_lease.valid()) {
    s_lease = s_power->acquire(s_ble ? watch::Res::BleFast : watch::Res::Wifi,
                               "ota");
  } else if (!active && s_lease.valid()) {
    s_lease.release();
  }
  // 変化があったときだけ Event にする (app タスクが拾って EVT/UI 更新)。
  static Status s_pub;
  if (s.stage != s_pub.stage || s.pct != s_pub.pct) {
    s_pub = s;
    if (s_bus) {
      s_bus->publish(
          {watch::EventType::OtaProgress, static_cast<uint32_t>(s.pct)});
    }
  }
}

void mark_valid_if_pending() {
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  if (esp_ota_get_state_partition(running, &st) == ESP_OK &&
      st == ESP_OTA_IMG_PENDING_VERIFY) {
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "self-check ok → app marked valid (%s)",
             esp_err_to_name(err));
  }
#endif
}

}  // namespace ota
