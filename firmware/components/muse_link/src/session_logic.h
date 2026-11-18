// muse_link_session_logic — Muse セッションの純粋ロジック (T55 Phase 2 ③)。
//
// ESP-IDF に依存しない純 C。状態機械・開始前チェック・ステータス JSON の組み立てを
// ここに置き、ホストテスト (test/test_muse_session.c) から検証する。
// IDF 側の実際の処理 (Wi-Fi/SDK 起動/タスク) は src/muse_session.cpp。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "muse_link/muse_link.h"  // muse_link_session_state_t / muse_link_start_error_t

#ifdef __cplusplus
extern "C" {
#endif

// ---- 状態機械 ----
typedef enum {
    MUSE_SESSION_EV_START = 0,   // muse_link_start() 受付
    MUSE_SESSION_EV_WIFI_OK,     // Wi-Fi 接続できた
    MUSE_SESSION_EV_WIFI_FAIL,   // Wi-Fi 接続失敗 (または資格情報なし)
    MUSE_SESSION_EV_CONNECTED,   // VM とセッション確立 (REACHABLE)
    MUSE_SESSION_EV_DISCONNECTED,// セッション切断 (UNREACHABLE 等)
    MUSE_SESSION_EV_TURN_BEGIN,  // 送信/受信開始 (busy)
    MUSE_SESSION_EV_TURN_DONE,   // 返答完了 (ready)
    MUSE_SESSION_EV_TURN_ERROR,  // ターン失敗 (error)
    MUSE_SESSION_EV_STOP,        // muse_link_stop()
} muse_session_event_t;

// 遷移後の状態。不正な遷移は現状維持 (ログ用に false を返す版もある)。
muse_link_session_state_t muse_link_session_advance(muse_link_session_state_t state,
                                                    muse_session_event_t ev, bool* changed);

// ---- 開始前チェック ----
// 資格情報 / Wi-Fi 設定 / 内蔵 SRAM / PSRAM を見て、開始してよいか判定する。
// 戻り値 MUSE_LINK_START_OK なら開始可。それ以外はエラー種別 + 日本語文言。
muse_link_start_error_t muse_link_session_check_start(bool configured, bool wifi_configured,
                                                      size_t internal_largest_free,
                                                      size_t psram_largest_free,
                                                      size_t min_internal_bytes,
                                                      size_t min_psram_bytes);

// エラー種別の日本語文言。秘密は含まない。
const char* muse_link_session_error_text(muse_link_start_error_t err);

// ---- ステータス JSON ----
// get_status() と同じ形の JSON を作る (純関数)。token は長さのみ。
typedef struct {
    bool configured;
    bool ready;
    bool wifi;
    bool wifi_owned;
    int wifi_state;            // muse_link_wifi_state_t
    muse_link_session_state_t session;
    size_t last_reply_len;     // 保持している返答の長さ (バイト)
    size_t total_reply_len;    // 受信した返答の総バイト数 (切り詰め前)
    const char* host;          // 秘密でない
    const char* vm;
    size_t token_len;
    const char* last_error;    // 日本語/英語の短い文言 (NULL 可)
} muse_link_status_fields_t;

bool muse_link_session_status_json(char* out, size_t cap,
                                   const muse_link_status_fields_t* f);

// 状態名 ("off" / "starting" / "ready" / "busy" / "error")。
const char* muse_link_session_state_name(muse_link_session_state_t st);

#ifdef __cplusplus
}
#endif
