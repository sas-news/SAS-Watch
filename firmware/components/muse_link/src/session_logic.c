// muse_link_session_logic — Muse セッションの純粋ロジック (T55 Phase 2 ③)。
// 純 C / ESP-IDF 非依存。ホストテストは test/test_muse_session.c。
#include "session_logic.h"

#include <stdio.h>
#include <string.h>

// ---- 状態機械 ----
muse_link_session_state_t muse_link_session_advance(muse_link_session_state_t state,
                                                    muse_session_event_t ev, bool* changed)
{
    muse_link_session_state_t next = state;

    switch (ev) {
        case MUSE_SESSION_EV_START:
            // どこからでも (再) 開始要求は STARTING へ。既に起動中/接続済みなら維持。
            if (state == MUSE_LINK_SESSION_OFF || state == MUSE_LINK_SESSION_ERROR) {
                next = MUSE_LINK_SESSION_STARTING;
            }
            break;

        case MUSE_SESSION_EV_WIFI_OK:
            if (state == MUSE_LINK_SESSION_STARTING) {
                next = MUSE_LINK_SESSION_STARTING;  // VM 接続待ちのまま
            }
            break;

        case MUSE_SESSION_EV_WIFI_FAIL:
            if (state == MUSE_LINK_SESSION_STARTING) {
                next = MUSE_LINK_SESSION_ERROR;
            }
            break;

        case MUSE_SESSION_EV_CONNECTED:
            if (state == MUSE_LINK_SESSION_STARTING) {
                next = MUSE_LINK_SESSION_READY;
            } else if (state == MUSE_LINK_SESSION_BUSY) {
                next = MUSE_LINK_SESSION_BUSY;  // ターン中は維持
            }
            break;

        case MUSE_SESSION_EV_DISCONNECTED:
            if (state == MUSE_LINK_SESSION_STARTING || state == MUSE_LINK_SESSION_READY ||
                state == MUSE_LINK_SESSION_BUSY) {
                next = MUSE_LINK_SESSION_ERROR;
            }
            break;

        case MUSE_SESSION_EV_TURN_BEGIN:
            if (state == MUSE_LINK_SESSION_READY || state == MUSE_LINK_SESSION_BUSY) {
                next = MUSE_LINK_SESSION_BUSY;
            }
            break;

        case MUSE_SESSION_EV_TURN_DONE:
            if (state == MUSE_LINK_SESSION_BUSY) {
                next = MUSE_LINK_SESSION_READY;
            }
            break;

        case MUSE_SESSION_EV_TURN_ERROR:
            if (state == MUSE_LINK_SESSION_BUSY || state == MUSE_LINK_SESSION_READY) {
                next = MUSE_LINK_SESSION_ERROR;
            }
            break;

        case MUSE_SESSION_EV_STOP:
            next = MUSE_LINK_SESSION_OFF;
            break;

        default:
            break;
    }

    if (changed) {
        *changed = next != state;
    }
    return next;
}

// ---- 開始前チェック ----
muse_link_start_error_t muse_link_session_check_start(bool configured, bool wifi_configured,
                                                      size_t internal_largest_free,
                                                      size_t psram_largest_free,
                                                      size_t min_internal_bytes,
                                                      size_t min_psram_bytes)
{
    if (!configured) {
        return MUSE_LINK_START_ERR_NOT_CONFIGURED;
    }
    if (!wifi_configured) {
        return MUSE_LINK_START_ERR_WIFI_NOT_CONFIGURED;
    }
    // 0 はチェック無効 (Kconfig で調整)。
    if (min_internal_bytes && internal_largest_free < min_internal_bytes) {
        return MUSE_LINK_START_ERR_NO_INTERNAL_RAM;
    }
    if (min_psram_bytes && psram_largest_free < min_psram_bytes) {
        return MUSE_LINK_START_ERR_NO_PSRAM;
    }
    return MUSE_LINK_START_OK;
}

const char* muse_link_session_error_text(muse_link_start_error_t err)
{
    switch (err) {
        case MUSE_LINK_START_OK:
            return "";
        case MUSE_LINK_START_ERR_NOT_CONFIGURED:
            return "Muse の資格情報が未設定です (muse set host/token)";
        case MUSE_LINK_START_ERR_WIFI_NOT_CONFIGURED:
            return "Wi-Fi が未設定です (スマホアプリで設定してください)";
        case MUSE_LINK_START_ERR_NO_INTERNAL_RAM:
            return "内蔵メモリ不足で Muse を開始できません";
        case MUSE_LINK_START_ERR_NO_PSRAM:
            return "PSRAM 不足で Muse を開始できません";
        case MUSE_LINK_START_ERR_WIFI_FAILED:
            return "Wi-Fi に接続できません";
        case MUSE_LINK_START_ERR_SESSION_START_FAILED:
            return "Muse セッションを開始できません";
        case MUSE_LINK_START_ERR_BUSY:
            return "Muse は既に起動しています";
    }
    return "Muse を開始できません";
}

// ---- ステータス JSON ----
static const char* state_name(muse_link_session_state_t st)
{
    return muse_link_session_state_name(st);
}

const char* muse_link_session_state_name(muse_link_session_state_t st)
{
    switch (st) {
        case MUSE_LINK_SESSION_OFF:
            return "off";
        case MUSE_LINK_SESSION_STARTING:
            return "starting";
        case MUSE_LINK_SESSION_READY:
            return "ready";
        case MUSE_LINK_SESSION_BUSY:
            return "busy";
        case MUSE_LINK_SESSION_ERROR:
            return "error";
    }
    return "off";
}

static void json_escape(const char* in, char* out, size_t cap)
{
    size_t o = 0;
    if (!in) {
        out[0] = '\0';
        return;
    }
    for (const char* p = in; *p && o + 2 < cap; ++p) {
        if (*p == '"' || *p == '\\') {
            out[o++] = '\\';
            out[o++] = *p;
        } else {
            out[o++] = ((unsigned char)*p < 0x20) ? '?' : *p;
        }
    }
    out[o] = '\0';
}

static const char* wifi_state_name(int st)
{
    switch (st) {
        case 0:
            return "off";
        case 1:
            return "starting";
        case 2:
            return "connected";
    }
    return "off";
}

bool muse_link_session_status_json(char* out, size_t cap, const muse_link_status_fields_t* f)
{
    if (!out || cap == 0 || !f) {
        return false;
    }
    char host_e[160];
    char vm_e[160];
    char err_e[256];
    json_escape(f->host, host_e, sizeof(host_e));
    json_escape(f->vm, vm_e, sizeof(vm_e));
    json_escape(f->last_error ? f->last_error : "", err_e, sizeof(err_e));

    const int n = snprintf(out, cap,
                           "{\"configured\":%s,\"ready\":%s,\"wifi\":%s,"
                           "\"wifi_state\":\"%s\",\"wifi_owned\":%s,"
                           "\"session\":\"%s\",\"last_reply_len\":%u,"
                           "\"total_reply_len\":%u,\"last_error\":\"%s\","
                           "\"host\":\"%s\",\"vm\":\"%s\",\"token_len\":%u}",
                           f->configured ? "true" : "false",
                           f->ready ? "true" : "false",
                           f->wifi ? "true" : "false",
                           wifi_state_name(f->wifi_state),
                           f->wifi_owned ? "true" : "false",
                           state_name(f->session),
                           (unsigned)f->last_reply_len, (unsigned)f->total_reply_len,
                           err_e, host_e, vm_e, (unsigned)f->token_len);
    return n > 0 && (size_t)n < cap;
}
