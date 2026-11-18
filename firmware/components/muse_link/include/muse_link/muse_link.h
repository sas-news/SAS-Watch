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

#include "muse_link/muse_link_voice.h"

#ifdef __cplusplus
extern "C" {
#endif

// Wi-Fi の状態 (T54 Phase 2 ②)。Muse は必要なときだけ Wi-Fi を上げる。
typedef enum {
    MUSE_LINK_WIFI_OFF = 0,   // 落ちている
    MUSE_LINK_WIFI_STARTING,  // 自分が session_start 中 (接続待ち)
    MUSE_LINK_WIFI_CONNECTED, // 接続済み (自分が上げた/他機能が上げた)
} muse_link_wifi_state_t;

// 状態 (ログ/UI 用。秘密は含めない)。
typedef struct {
    bool configured;   // host と token が揃っている
    bool ready;        // configured && Wi-Fi 接続中
    bool wifi;         // Wi-Fi 接続中か
    bool wifi_owned;   // このコンポーネントが session_start したか (release 対象)
    int wifi_state;    // muse_link_wifi_state_t
    char host[64];     // NVS "muse"/"host" (未設定なら既定ホスト)
    char vm_id[64];    // NVS "muse"/"vm" (空 = アカウント既定 VM)
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

// ---- Wi-Fi on-demand (T54 Phase 2 ②) ----
// 方針: Muse は使うときだけ Wi-Fi を上げ、使い終わったら完全に落とす
// (常時接続しない: SAS-Watch の電池方針)。他機能 (OTA 等) が上げている
// Wi-Fi は落とさない (所有権はこのコンポーネント側で管理)。
// **app タスクから呼ぶこと** (LVGL タスクから呼ばない。session_start は
// 最大 timeout_ms ブロックし、Wi-Fi/BLE のヒープを使う)。

// 必要なら Wi-Fi を上げる。既に接続済みなら即 true。
// timeout_ms は session_start の待ち時間。資格情報が無ければ false。
bool muse_link_wifi_ensure(uint32_t timeout_ms);

// 自分が上げた Wi-Fi を落とす。他機能が上げている場合は何もしない。
void muse_link_wifi_release(void);

// アクティビティを記録してアイドルタイマーをリセットする (送受信のたび)。
void muse_link_wifi_touch(void);

// アイドル (既定 60 秒, CONFIG_WATCH_MUSE_WIFI_IDLE_S) を過ぎたら release。
// 呼び出しは app タスクのループから (Phase 2 ③ でセッションと一緒に回す)。
void muse_link_wifi_poll(void);

// テスト用: 時刻を注入してアイドル判定だけを行う。
void muse_link_wifi_poll_at(uint32_t now_ms);

// 現在の Wi-Fi 状態/所有権/アイドル秒。
muse_link_wifi_state_t muse_link_wifi_state(void);
bool muse_link_wifi_owned(void);
uint32_t muse_link_wifi_idle_ms(void);

// ---- 音声ノートの変換 (T54 Phase 2 ④) ----
// 16kHz/16bit/mono PCM → WAV/base64 (SDK の音声ノート形式)。
// 逐次変換のみ (PCM 全部・base64 全部を一度に持たない)。上限 60 秒。
// 実送信は Phase 2 ③。API は muse_link_voice.h を参照。

// ---- チャット (未実装) ----

// テキストを Muse に送る。Phase 1/2① は未実装 (常に ESP_ERR_NOT_SUPPORTED)。
// TODO(muse): Phase 2 で chat セッション (POST /chat/stream + /chat/subscribe)
// につなぎ、応答の断片を callback で返す。
typedef void (*muse_link_chat_cb)(const char* text, bool done, void* ctx);
esp_err_t muse_link_chat(const char* text, muse_link_chat_cb cb, void* ctx);

#ifdef __cplusplus
}
#endif
