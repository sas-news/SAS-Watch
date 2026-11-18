// muse_link — Muse chat/Noise 層の土台 + 資格情報の投入 (T50 Phase 1 / T52 Phase 2 ①)。
//
// Phase 1: 起動時に NVS (namespace "muse") の host / vm / token を読んで保持。
// T52 追加: シリアル `muse` コマンドからの投入・確認・消去 (muse_link_console_line)。
// 実接続 (TLS/Noise セッション) は Phase 2 の後半。TODO(muse)。
//
// NVS スキーマは SDK の `components/muse/muse_settings.c` に合わせる:
//   namespace "muse", キー "host" / "vm" / "token" (token は 1023B まで)。
// 旧 Phase 1 の "vm_id" は後方互換のため読み出しだけ受け付ける。
//
// 秘密の扱い: token の値はログ・UI・シリアル出力に一切出さない (長さだけ)。
// NVS には平文で入る (T52 時点)。暗号化は Phase 2 の課題 (docs/muse-bmin.md)。
#include "muse_link/muse_link.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "nvs.h"

#include "muse_console_parse.h"
#include "muse_link_internal.h"
#include "wifi/wifi.hpp"

namespace {

constexpr const char* kTag = "muse_link";

// NVS namespace とキー (SDK の muse_settings.c と同じ)。
constexpr const char* kNvsNamespace = "muse";
constexpr const char* kKeyHost = "host";
constexpr const char* kKeyVm = "vm";
constexpr const char* kKeyVmLegacy = "vm_id";  // Phase 1 の旧キー (読むだけ)
constexpr const char* kKeyToken = "token";

// SDK の既定ホスト (muse_chat_session.cpp:960)。
constexpr const char* kDefaultHost = "hatch.metaaivm.com";

// 保持領域。上限は SDK の muse_settings.h と同じ (63 / 63 / 1023 + NUL)。
// Kconfig 有効時だけコンパイルされるので、無効ビルドには一切乗らない。
char s_host[MUSE_HOST_MAX + 1];
char s_vm[MUSE_VM_MAX + 1];
// token (最大 1024B) は PSRAM に置く。内蔵 SRAM は常に逼迫しているため
// (AGENTS.md のメモリ予算)。読み書きは init/provision 時と SDK 側コピーのみ。
EXT_RAM_BSS_ATTR char s_token[MUSE_TOKEN_MAX + 1];
size_t s_token_len = 0;

// muse_link_console_line の値コピー先 (token 1023B をスタックに置かない)。
// シリアルタスクからのみ呼ばれる前提 (docs/muse-bmin.md)。こちらも PSRAM。
EXT_RAM_BSS_ATTR char s_console_value[MUSE_TOKEN_MAX + 1];

// nvs_get_str は「バッファ不足」をエラーで返すので、長さを問い合わせてから読む。
void read_str(nvs_handle_t h, const char* key, char* out, size_t cap) {
    size_t len = 0;
    esp_err_t err = nvs_get_str(h, key, nullptr, &len);
    if (err != ESP_OK || len == 0 || len > cap) {
        return;  // 未設定・空・長すぎる: out はそのまま
    }
    if (nvs_get_str(h, key, out, &len) != ESP_OK) {
        out[0] = '\0';
    }
}

// 保存前の検証: 長さ上限 + 制御文字/空白/非 ASCII を拒否する。
// token は不透明文字列だが、シリアル入力と NVS の扱いを単純にするため
// 「印字可能な非空白 ASCII」に限定する (TODO(muse): 必要なら緩和)。
bool valid_value(const char* what, const char* v, size_t max_len) {
    const size_t n = strlen(v);
    if (n > max_len) {
        ESP_LOGW(kTag, "%s too long (%u > %u bytes)", what, (unsigned)n,
                 (unsigned)max_len);
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(v[i]);
        if (c <= 0x20 || c >= 0x7f) {
            ESP_LOGW(kTag, "%s contains a space/control/non-ASCII byte (pos %u)",
                     what, (unsigned)i);
            return false;
        }
    }
    return true;
}

// NULL = 変更しない / "" = そのキーを消す / それ以外 = 保存する。
bool write_key(nvs_handle_t h, const char* key, const char* v) {
    if (!v || !*v) {
        esp_err_t err = nvs_erase_key(h, key);
        return err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND;
    }
    return nvs_set_str(h, key, v) == ESP_OK;
}

void reload_cache(void) {
    s_host[0] = '\0';
    s_vm[0] = '\0';
    s_token[0] = '\0';
    s_token_len = 0;

    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) {
        strncpy(s_host, kDefaultHost, sizeof(s_host) - 1);
        return;
    }
    read_str(h, kKeyHost, s_host, sizeof(s_host));
    read_str(h, kKeyVm, s_vm, sizeof(s_vm));
    if (!s_vm[0]) {
        read_str(h, kKeyVmLegacy, s_vm, sizeof(s_vm));  // Phase 1 の旧キー
    }
    read_str(h, kKeyToken, s_token, sizeof(s_token));
    nvs_close(h);

    if (!s_host[0]) {
        strncpy(s_host, kDefaultHost, sizeof(s_host) - 1);
        s_host[sizeof(s_host) - 1] = '\0';
    }
    s_token_len = strlen(s_token);
}

// JSON 文字列用の最小エスケープ (host/vm は検証済みだが念のため)。
void json_escape(const char* in, char* out, size_t cap) {
    size_t o = 0;
    for (const char* p = in; *p && o + 2 < cap; ++p) {
        if (*p == '"' || *p == '\\') {
            out[o++] = '\\';
            out[o++] = *p;
        } else {
            out[o++] = (*p < 0x20) ? '?' : *p;
        }
    }
    out[o] = '\0';
}

}  // namespace

// ---- 公開 API ----

extern "C" void muse_link_keep_linked(void);  // muse_adapters.cpp (Phase 1 のリンク維持)

extern "C" esp_err_t muse_link_init(void) {
    reload_cache();
    // Phase 1/2①: 秘密は出さない (token は長さだけ)。host/vm は識別子なので出す。
    ESP_LOGI(kTag, "host=%s vm=%s token=%s (len %u)", s_host,
             s_vm[0] ? s_vm : "(default)", s_token_len ? "set" : "unset",
             static_cast<unsigned>(s_token_len));
    ESP_LOGI(kTag, "foundation only (no network, no session task)");
    muse_link_keep_linked();  // Phase 1: chat オブジェクトをリンクに残すだけ (実行しない)
    return ESP_OK;
}

extern "C" bool muse_link_provision(const char* host, const char* vm, const char* token) {
    // 先に全部検証する (一部だけ保存される事故を防ぐ)。
    if (host && !valid_value("host", host, MUSE_HOST_MAX)) {
        return false;
    }
    if (vm && !valid_value("vm", vm, MUSE_VM_MAX)) {
        return false;
    }
    if (token && !valid_value("token", token, MUSE_TOKEN_MAX)) {
        return false;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }
    bool ok = true;
    if (host) {
        ok = write_key(h, kKeyHost, host) && ok;
    }
    if (vm) {
        ok = write_key(h, kKeyVm, vm) && ok;
        if (ok && !*vm) {
            (void)nvs_erase_key(h, kKeyVmLegacy);  // 旧キーも消しておく
        }
    }
    if (token) {
        ok = write_key(h, kKeyToken, token) && ok;
    }
    if (ok) {
        err = nvs_commit(h);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "nvs_commit failed: %s", esp_err_to_name(err));
            ok = false;
        }
    }
    nvs_close(h);

    if (!ok) {
        ESP_LOGE(kTag, "provision failed (NVS write)");
        return false;
    }
    reload_cache();
    // 値は host/vm のみ。token は長さだけ。
    if (host) {
        ESP_LOGI(kTag, "provision: host=%s", s_host);
    }
    if (vm) {
        ESP_LOGI(kTag, "provision: vm=%s", s_vm[0] ? s_vm : "(cleared)");
    }
    if (token) {
        ESP_LOGI(kTag, "provision: token=%s (len %u)",
                 s_token_len ? "set" : "cleared", static_cast<unsigned>(s_token_len));
    }
    return true;
}

extern "C" void muse_link_clear(void) {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) == ESP_OK) {
        esp_err_t err = nvs_erase_all(h);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
        if (err == ESP_OK) {
            ESP_LOGI(kTag, "cleared NVS namespace \"%s\"", kNvsNamespace);
        } else {
            ESP_LOGE(kTag, "clear failed: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGI(kTag, "nothing to clear (namespace absent)");
    }
    reload_cache();
}

extern "C" bool muse_link_get_status(char* out, size_t cap) {
    if (!out || cap == 0) {
        return false;
    }
    char host_e[2 * sizeof(s_host)];
    char vm_e[2 * sizeof(s_vm)];
    json_escape(s_host, host_e, sizeof(host_e));
    json_escape(s_vm, vm_e, sizeof(vm_e));
    const bool configured = s_token_len > 0 && s_host[0] != '\0';
    const bool wifi = wifi::connected();
    const int n = snprintf(out, cap,
                           "{\"configured\":%s,\"ready\":%s,\"wifi\":%s,"
                           "\"host\":\"%s\",\"vm\":\"%s\",\"token_len\":%u}",
                           configured ? "true" : "false",
                           (configured && wifi) ? "true" : "false",
                           wifi ? "true" : "false", host_e, vm_e,
                           static_cast<unsigned>(s_token_len));
    return n > 0 && static_cast<size_t>(n) < cap;
}

extern "C" bool muse_link_ready(void) {
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
    // TODO(muse): Phase 2 後半で chat セッション (POST /chat/stream +
    // /chat/subscribe) につなぐ。現状は土台のみ。
    static bool warned = false;
    if (!warned) {
        warned = true;
        ESP_LOGW(kTag, "chat is not implemented yet; ESP_ERR_NOT_SUPPORTED");
    }
    return ESP_ERR_NOT_SUPPORTED;
}

// ---- シリアル `muse` コマンド (T52) ----
// watch_app の serial_dbg_task が 1 行を渡すだけ。値は一切ログに出さない
// (token は長さのみ)。呼び出しはシリアルタスクからのみ (s_console_value を使う)。
extern "C" bool muse_link_console_line(const char* line) {
    muse_console_parsed_t p;
    if (!muse_console_parse(line, &p)) {
        ESP_LOGW(kTag, "unknown muse command (see 'M' then 'muse help')");
        return false;
    }

    switch (p.cmd) {
        case MUSE_CONSOLE_HELP:
            ESP_LOGI(kTag,
                     "usage (line mode: send 'M', then the line + Enter): "
                     "muse status | muse set host <host> | muse set vm <vm> | "
                     "muse set token <token> | muse clear");
            return true;

        case MUSE_CONSOLE_STATUS: {
            char buf[256];
            if (muse_link_get_status(buf, sizeof(buf))) {
                ESP_LOGI(kTag, "status %s", buf);
            } else {
                ESP_LOGE(kTag, "status buffer too small");
            }
            return true;
        }

        case MUSE_CONSOLE_CLEAR:
            muse_link_clear();
            return true;

        case MUSE_CONSOLE_SET_HOST:
        case MUSE_CONSOLE_SET_VM:
        case MUSE_CONSOLE_SET_TOKEN: {
            // 1023B の token をスタックに置かない (sdbg タスクは 4KB スタック)。
            const size_t n = p.value_len < sizeof(s_console_value) - 1
                                 ? p.value_len
                                 : sizeof(s_console_value) - 1;
            memcpy(s_console_value, p.value, n);
            s_console_value[n] = '\0';
            if (p.value_len >= sizeof(s_console_value)) {
                ESP_LOGW(kTag, "value too long (max %u)", (unsigned)(sizeof(s_console_value) - 1));
                return false;
            }
            bool ok = false;
            if (p.cmd == MUSE_CONSOLE_SET_HOST) {
                ok = muse_link_provision(s_console_value, nullptr, nullptr);
            } else if (p.cmd == MUSE_CONSOLE_SET_VM) {
                ok = muse_link_provision(nullptr, s_console_value, nullptr);
            } else {
                // token の値はここでも出さない (長さだけ provision がログする)。
                ESP_LOGI(kTag, "set token: %u bytes", (unsigned)n);
                ok = muse_link_provision(nullptr, nullptr, s_console_value);
            }
            memset(s_console_value, 0, sizeof(s_console_value));
            return ok;
        }

        default:
            ESP_LOGW(kTag, "unknown muse command");
            return false;
    }
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
