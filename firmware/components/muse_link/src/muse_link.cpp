// muse_link — Muse chat/Noise 層の土台 (T50 Phase 1)。
//
// Phase 1 でやること:
//   - NVS (namespace "muse") の host / vm_id / token を読んで保持する。
//   - 状態をログに出す (token は長さだけ。値は出さない)。
// やらないこと (Phase 2):
//   - Wi-Fi 接続、TLS/Noise セッション、セッションタスク、PSRAM の大確保、
//     UI (字幕) への配線。TODO(muse): M1 の資格情報が入ったら Phase 2。
//
// 秘密の扱い: token は NVS の "muse"/"token" にのみ存在し、ログ・UI には出さない。
// NVS 暗号化は現状未有効 (docs/muse-bmin.md リスク参照)。
#include "muse_link/muse_link.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "muse_link_internal.h"
#include "wifi/wifi.hpp"

namespace {

constexpr const char* kTag = "muse_link";

// NVS namespace とキー (docs/muse-bmin.md のスキーマ)。
// 書き込みは Phase 2 で BLE settings.set / シリアルコマンドから行う。
constexpr const char* kNvsNamespace = "muse";
constexpr const char* kKeyHost = "host";
constexpr const char* kKeyVmId = "vm_id";
constexpr const char* kKeyToken = "token";

// SDK の既定ホスト (components/muse/muse_chat_session.cpp:960)。
// NVS の "host" があればそちらを使う。
constexpr const char* kDefaultHost = "hatch.metaaivm.com";

// 保持領域。上限は SDK の muse_settings.h と同じ (63 / 63 / 1023 + NUL)。
// Kconfig 有効時だけコンパイルされるので、無効ビルドには一切乗らない。
char s_host[MUSE_HOST_MAX + 1];
char s_vm[MUSE_VM_MAX + 1];
char s_token[MUSE_TOKEN_MAX + 1];
size_t s_token_len = 0;

// nvs_get_str は「バッファ不足」をエラーで返すので、長さを問い合わせてから読む。
void read_str(nvs_handle_t h, const char* key, char* out, size_t cap) {
    size_t len = 0;
    esp_err_t err = nvs_get_str(h, key, nullptr, &len);
    if (err != ESP_OK || len == 0 || len > cap) {
        return;  // 未設定・空・長すぎる: out はそのまま (呼び出し側で既定値を入れる)
    }
    if (nvs_get_str(h, key, out, &len) != ESP_OK) {
        out[0] = '\0';
    }
}

}  // namespace

extern "C" void muse_link_keep_linked(void);  // muse_adapters.cpp (Phase 1 のリンク維持)

extern "C" esp_err_t muse_link_init(void) {
    // 既定値 (NVS 未設定時のフォールバック)。
    strncpy(s_host, kDefaultHost, sizeof(s_host) - 1);
    s_host[sizeof(s_host) - 1] = '\0';
    s_vm[0] = '\0';
    s_token[0] = '\0';
    s_token_len = 0;

    nvs_handle_t h;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // namespace が無い = まだ資格情報を入れていない。異常ではない。
        ESP_LOGI(kTag, "phase1: no credentials (NVS \"%s\" not found); host default %s",
                 kNvsNamespace, s_host);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "phase1: nvs_open(\"%s\") failed: %s", kNvsNamespace,
                 esp_err_to_name(err));
        return err;
    }

    read_str(h, kKeyHost, s_host, sizeof(s_host));
    read_str(h, kKeyVmId, s_vm, sizeof(s_vm));
    read_str(h, kKeyToken, s_token, sizeof(s_token));
    nvs_close(h);

    s_token_len = strlen(s_token);

    // 秘密は出さない (token は長さだけ)。host/vm_id は識別子なので出してよい。
    ESP_LOGI(kTag, "phase1: host=%s vm=%s token=%s (len %u)",
             s_host, s_vm[0] ? s_vm : "(default)",
             s_token_len ? "set" : "unset", static_cast<unsigned>(s_token_len));
    muse_link_keep_linked();  // Phase 1: chat オブジェクトをリンクに残すだけ (実行しない)
    ESP_LOGI(kTag, "phase1: foundation only (no network, no session task)");
    return ESP_OK;
}

extern "C" bool muse_link_ready(void) {
    // Phase 1: 「設定済み かつ Wi-Fi 接続中」かどうかだけ。接続はしない。
    return s_token_len > 0 && s_host[0] != '\0' && wifi::connected();
}

extern "C" void muse_link_status(muse_link_status_t* out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->configured = s_token_len > 0 && s_host[0] != '\0';
    out->wifi = wifi::connected();
    out->ready = out->configured && out->wifi;
    strncpy(out->host, s_host, sizeof(out->host) - 1);
    strncpy(out->vm_id, s_vm, sizeof(out->vm_id) - 1);
}

extern "C" esp_err_t muse_link_chat(const char* text, muse_link_chat_cb cb, void* ctx) {
    (void)text;
    (void)cb;
    (void)ctx;
    // TODO(muse): Phase 2 で chat セッション (POST /chat/stream + /chat/subscribe)
    // につなぐ。Phase 1 は土台のみなので常に未対応を返す。
    static bool warned = false;
    if (!warned) {
        warned = true;
        ESP_LOGW(kTag, "chat is Phase 2; returning ESP_ERR_NOT_SUPPORTED");
    }
    return ESP_ERR_NOT_SUPPORTED;
}

// ---- 内部共有 (muse_adapters.cpp が SDK の muse_settings_* として公開する) ----

extern "C" void muse_link_cfg_host(char out[MUSE_HOST_MAX + 1]) {
    strncpy(out, s_host, MUSE_HOST_MAX);
    out[MUSE_HOST_MAX] = '\0';
}

extern "C" void muse_link_cfg_vm(char out[MUSE_VM_MAX + 1]) {
    strncpy(out, s_vm, MUSE_VM_MAX);
    out[MUSE_VM_MAX] = '\0';
}

extern "C" void muse_link_cfg_token(char out[MUSE_TOKEN_MAX + 1]) {
    strncpy(out, s_token, MUSE_TOKEN_MAX);
    out[MUSE_TOKEN_MAX] = '\0';
}

extern "C" size_t muse_link_cfg_token_len(void) {
    return s_token_len;
}
