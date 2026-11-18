// muse_link — Muse chat/Noise 層の SAS-Watch 向け公開 API (T50 Phase 1)。
//
// Phase 1 の範囲:
//   - 起動時に NVS (namespace "muse") の host / vm_id / token を読むだけ。
//   - ネットワーク接続・タスク生成・PSRAM の大確保はしない (Phase 2)。
//   - 資格情報が未設定なら全 API が no-op / false を返す。
//
// Phase 2 でセッション開始 (実接続)、音声アダプタ、UI (字幕) を足す。
// 設計と実測は docs/muse-bmin.md / docs/muse-port.md / docs/muse-chat-spike.md。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 状態 (ログ/UI 用。秘密は含めない)。
typedef struct {
    bool configured;   // host と token が揃っている
    bool ready;        // configured && Wi-Fi 接続中 (Phase 1 では通常 false)
    bool wifi;         // Wi-Fi 接続中か
    char host[64];     // NVS "muse"/"host" (未設定なら既定ホスト)
    char vm_id[64];    // NVS "muse"/"vm_id" (空 = アカウント既定 VM)
} muse_link_status_t;

// NVS から資格情報を読む。ネットワークには一切触らない。
// 何度呼んでも安全 (Phase 1 では2回目以降も読み直す)。
esp_err_t muse_link_init(void);

// 資格情報が揃っていて、Phase 2 のセッションを開始できる状態か。
// Phase 1 では「設定済み && Wi-Fi 接続中」を返すが、接続自体はしない。
bool muse_link_ready(void);

// 現在の状態を書き出す (秘密は含まない)。
void muse_link_status(muse_link_status_t* out);

// ---- 資格情報の投入・確認・消去 (T52 Phase 2 ①) ----
// NVS namespace "muse"、キー "host" / "vm" / "token" (SDK の muse_settings.c と同じ)。
// token の値はログ・シリアル・UI に一切出さない (長さだけ)。
// T52 時点は NVS に平文で保存する (暗号化は Phase 2 の課題。docs/muse-bmin.md)。

// token の最大長 (SDK の MUSE_TOKEN_MAX と同じ 1023)。
#define MUSE_LINK_TOKEN_MAX 1023

// 資格情報を保存する。NULL = その項目は変更しない / "" = その項目を消す。
// すべての値は事前検証され、1 つでも不正なら何も保存しない。
// 各上限: host 63B / vm 63B / token 1023B。印字可能な非空白 ASCII のみ。
bool muse_link_provision(const char* host, const char* vm, const char* token);

// NVS namespace "muse" を消す (host/vm/token すべて)。再起動は不要。
void muse_link_clear(void);

// 状態を 1 行の JSON で返す。token は長さだけ。
// 例: {"configured":true,"ready":false,"wifi":false,"host":"...","vm":"...","token_len":16}
// バッファ不足なら false。
bool muse_link_get_status(char* out, size_t cap);

// シリアルから渡された 1 行 (`muse status` など) を解釈して実行する。
// watch_app の serial_dbg_task 専用 (値を静的バッファにコピーするため単一呼び出し元)。
// コマンド語: muse status | muse set host <v> | muse set vm <v> |
//             muse set token <v> | muse clear | muse help
// 値は一切ログに出さない (token は長さだけ)。
bool muse_link_console_line(const char* line);

// ---- チャット (未実装) ----

// テキストを Muse に送る。Phase 1/2① は未実装 (常に ESP_ERR_NOT_SUPPORTED)。
// TODO(muse): Phase 2 で chat セッション (POST /chat/stream + /chat/subscribe)
// につなぎ、応答の断片を callback で返す。
typedef void (*muse_link_chat_cb)(const char* text, bool done, void* ctx);
esp_err_t muse_link_chat(const char* text, muse_link_chat_cb cb, void* ctx);

#ifdef __cplusplus
}
#endif
