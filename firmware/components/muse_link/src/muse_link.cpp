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
#include "muse_voice_logic.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "muse_console_parse.h"
#include "session_logic.h"
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

// ---- Wi-Fi on-demand の状態 (T54 Phase 2 ②) ----
// 所有権: 自分が session_start したときだけ s_wifi_owned=true。release は
// そのときだけ session_end する (OTA 等が上げている Wi-Fi は落とさない)。
// 呼び出しは app タスク前提 (LVGL から呼ばない)。
bool s_wifi_owned = false;
uint32_t s_wifi_last_ms = 0;

#ifndef CONFIG_WATCH_MUSE_WIFI_IDLE_S
#define CONFIG_WATCH_MUSE_WIFI_IDLE_S 60  /* Kconfig off 時 (このファイルは未コンパイル) */
#endif
constexpr uint32_t kIdleMs = (uint32_t)CONFIG_WATCH_MUSE_WIFI_IDLE_S * 1000u;

uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

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

extern "C" esp_err_t muse_link_init(void) {
    reload_cache();
    // Phase 1/2①: 秘密は出さない (token は長さだけ)。host/vm は識別子なので出す。
    ESP_LOGI(kTag, "host=%s vm=%s token=%s (len %u)", s_host,
             s_vm[0] ? s_vm : "(default)", s_token_len ? "set" : "unset",
             static_cast<unsigned>(s_token_len));
    ESP_LOGI(kTag, "foundation only (no network, no session task)");
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
    muse_link_status_fields_t f = {};
    f.configured = s_token_len > 0 && s_host[0] != '\0';
    f.wifi = wifi::connected();
    f.wifi_owned = s_wifi_owned;
    f.wifi_state = (int)(f.wifi ? MUSE_LINK_WIFI_CONNECTED
                                : (s_wifi_owned ? MUSE_LINK_WIFI_STARTING
                                                : MUSE_LINK_WIFI_OFF));
    f.ready = f.configured && f.wifi;
    f.host = s_host;
    f.vm = s_vm;
    f.token_len = s_token_len;
    // 音声入力 (T57) の状態。音声そのものは含めない。
    f.voice_state = muse_voice_state_name((int)muse_link_voice_state());
    f.voice_elapsed_s = muse_link_voice_elapsed_s();
    f.voice_error = muse_link_voice_error();
    muse_link_session_fill_status(&f);
    return muse_link_session_status_json(out, cap, &f);
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
    out->wifi_owned = s_wifi_owned;
    out->wifi_state = (int)(out->wifi ? MUSE_LINK_WIFI_CONNECTED
                                     : (s_wifi_owned ? MUSE_LINK_WIFI_STARTING
                                                     : MUSE_LINK_WIFI_OFF));
    out->ready = out->configured && out->wifi;
    out->session_state = (int)muse_link_session_state();
    out->last_reply_len = muse_link_total_reply_len();
    out->voice_state = (int)muse_link_voice_state();
    out->voice_elapsed_s = muse_link_voice_elapsed_s();
    strncpy(out->voice_error, muse_link_voice_error(),
            sizeof(out->voice_error) - 1);
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

// ---- Wi-Fi on-demand (T54 Phase 2 ②) ----

extern "C" bool muse_link_wifi_ensure(uint32_t timeout_ms) {
    s_wifi_last_ms = now_ms();

    // 自分が上げた Wi-Fi がまだ生きている。
    if (s_wifi_owned) {
        if (wifi::connected()) {
            return true;
        }
        // 落ちた: いったん片付けて上げ直す。
        ESP_LOGW(kTag, "wifi dropped; restarting session");
        wifi::session_end();
        s_wifi_owned = false;
    } else if (wifi::connected()) {
        // OTA など他機能が上げている。所有権は取らない (release で落とさない)。
        ESP_LOGI(kTag, "wifi already up (owned by another user); not taking ownership");
        return true;
    }

    if (!wifi::configured()) {
        ESP_LOGW(kTag, "wifi not configured (no ssid)");
        return false;
    }
    ESP_LOGI(kTag, "wifi session_start (timeout %u ms)", (unsigned)timeout_ms);
    if (!wifi::session_start(timeout_ms)) {
        ESP_LOGW(kTag, "wifi session_start failed");
        s_wifi_owned = false;
        return false;
    }
    s_wifi_owned = true;
    s_wifi_last_ms = now_ms();
    ESP_LOGI(kTag, "wifi connected (owned by muse_link)");
    return true;
}

extern "C" void muse_link_wifi_release(void) {
    if (!s_wifi_owned) {
        ESP_LOGI(kTag, "wifi release: not owned (left to the other user)");
        return;
    }
    s_wifi_owned = false;
    wifi::session_end();
    ESP_LOGI(kTag, "wifi released (session_end)");
}

extern "C" void muse_link_wifi_touch(void) {
    if (s_wifi_owned) {
        s_wifi_last_ms = now_ms();
    }
}

extern "C" void muse_link_wifi_poll_at(uint32_t now_ms_arg) {
    if (!s_wifi_owned) {
        return;
    }
    if (wifi::connected()) {
        if ((uint32_t)(now_ms_arg - s_wifi_last_ms) < kIdleMs) {
            return;
        }
        ESP_LOGI(kTag, "wifi idle %u s; releasing",
                 (unsigned)(kIdleMs / 1000u));
    } else {
        ESP_LOGW(kTag, "wifi gone while owned; cleaning up");
    }
    muse_link_wifi_release();
}

extern "C" void muse_link_wifi_poll(void) {
    muse_link_wifi_poll_at(now_ms());
}

extern "C" muse_link_wifi_state_t muse_link_wifi_state(void) {
    if (wifi::connected()) {
        return MUSE_LINK_WIFI_CONNECTED;
    }
    return s_wifi_owned ? MUSE_LINK_WIFI_STARTING : MUSE_LINK_WIFI_OFF;
}

extern "C" bool muse_link_wifi_owned(void) {
    return s_wifi_owned;
}

extern "C" uint32_t muse_link_wifi_idle_ms(void) {
    return kIdleMs;
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
            // T57: voice の状態/エラーを足したので 512B 必要 (256 では不足)。
            char buf[512];
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

        case MUSE_CONSOLE_START: {
            const bool ok = muse_link_start(20000);
            ESP_LOGI(kTag, "console: muse start -> %s (session=%s)", ok ? "ok" : "failed",
                     muse_link_session_state_name(muse_link_session_state()));
            if (!ok) {
                ESP_LOGW(kTag, "console: %s", muse_link_last_error());
            }
            return ok;
        }

        case MUSE_CONSOLE_STOP:
            muse_link_stop();
            ESP_LOGI(kTag, "console: muse stop");
            return true;

        case MUSE_CONSOLE_REPLY:
            // 本文は出さない (長さだけ)。
            ESP_LOGI(kTag, "console: last reply %u bytes (kept %u)",
                     (unsigned)muse_link_total_reply_len(),
                     (unsigned)muse_link_last_reply_len());
            return true;

        case MUSE_CONSOLE_SEND: {
            if (p.value_len == 0) {
                ESP_LOGW(kTag, "console: muse send needs text");
                return false;
            }
            const size_t n = p.value_len < sizeof(s_console_value) - 1
                                 ? p.value_len
                                 : sizeof(s_console_value) - 1;
            memcpy(s_console_value, p.value, n);
            s_console_value[n] = '\0';
            if (p.value_len >= sizeof(s_console_value)) {
                ESP_LOGW(kTag, "console: text too long (max %u)",
                         (unsigned)(sizeof(s_console_value) - 1));
                memset(s_console_value, 0, sizeof(s_console_value));
                return false;
            }
            // 本文はログに出さない。長さだけ。
            ESP_LOGI(kTag, "console: muse send %u bytes", (unsigned)n);
            const bool ok = muse_link_send_text(s_console_value);
            memset(s_console_value, 0, sizeof(s_console_value));
            return ok;
        }

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

extern "C" void muse_link_cfg_get(char* host, size_t host_cap, char* vm, size_t vm_cap,
                                  size_t* token_len) {
    if (host && host_cap) {
        strncpy(host, s_host, host_cap - 1);
        host[host_cap - 1] = '\0';
    }
    if (vm && vm_cap) {
        strncpy(vm, s_vm, vm_cap - 1);
        vm[vm_cap - 1] = '\0';
    }
    if (token_len) {
        *token_len = s_token_len;
    }
}
