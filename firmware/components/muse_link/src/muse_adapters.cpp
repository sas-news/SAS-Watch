// muse_adapters — SDK chat 層が要求する SAS-Watch 側の実装 (T50 Phase 1)。
//
// T49 のスパイクで洗い出した 20 関数 + 埋め込み MP3 のシンボル 2 個を、
// SAS-Watch の既存コンポーネント (NVS / wifi / 字幕) で実装する。
// 未確定のものは TODO(hw) / TODO(muse) を付け、Phase 1 は safe な既定値で返す。
//
// 呼び出し元は SDK の chat セッションタスク (Phase 2 で有効化)。
// Phase 1 ではセッションタスクを起動しないため、ここは原則呼ばれない。
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// SDK のヘッダは extern "C" を付けないものが混在している (muse_settings.h など)。
// SDK の muse_chat_session.cpp と同じく、C リンケージが要るものはここで囲む。
extern "C" {
#include "muse_chat.h"
#include "muse_link.h"        // SDK の muse_link.h (Link セッション; SDK 側)
#include "muse_options.h"
#include "muse_settings.h"
#include "muse_state.h"
}

#include "muse_link/muse_link.h"  // このコンポーネントの公開 API
#include "muse_link_internal.h"
#include "wifi/wifi.hpp"

// ---- muse_settings: 設定 (NVS "muse") ----
// Phase 1 は muse_link_init() が読んだキャッシュを返すだけ。
// TODO(muse): Phase 2 で設定画面 / BLE settings.set から更新できるようにする。

void muse_settings_hatch_host(char out[MUSE_HOST_MAX + 1]) {
    muse_link_cfg_host(out);
}

void muse_settings_hatch_vm(char out[MUSE_VM_MAX + 1]) {
    muse_link_cfg_vm(out);
}

void muse_settings_hatch_token(char out[MUSE_TOKEN_MAX + 1]) {
    muse_link_cfg_token(out);
}

size_t muse_settings_hatch_token_len(void) {
    return muse_link_cfg_token_len();
}

// ---- muse_link: Home Link のアカウント/VM ----
// SAS-Watch は B-min で Muse アプリの BLE ペアリング (Link) を使わない方針
// (docs/muse-port.md)。device token は NVS "muse"/"token" に直接入れる。
// TODO(muse): M1 で device token が得られない場合は B-full (ペアリング移植) を検討。

bool muse_link_hatch_linked(void) {
    return false;
}

bool muse_link_hatch_vm(const char* want_vm, char* vm_id, size_t id_cap,
                                   char* vm_name, size_t name_cap, char** vm_token) {
    (void)want_vm;
    if (vm_id && id_cap) vm_id[0] = '\0';
    if (vm_name && name_cap) vm_name[0] = '\0';
    if (vm_token) *vm_token = nullptr;
    return false;
}

// ---- muse_wifi: 接続状態 ----
// SAS-Watch の wifi は「使うときだけ session_start」なので、Phase 1 では
// 原則 false。TODO(hw): Phase 2 でセッション開始中の状態を返す。

bool muse_wifi_connected(void) {
    return wifi::connected();
}

// ---- muse_options: Muse の選択肢 UI ----
// B-min は字幕のみ (選択肢 UI はスコープ外)。全部 no-op。
// TODO(muse): Muse が選択肢を返すケースを扱うなら Phase 3 で実装。

bool muse_options_snapshot(muse_options_t* out) {
    (void)out;
    return false;
}

void muse_options_clear(void) {}

void muse_options_disconnected(void) {}

bool muse_options_pending(void) {
    return false;
}

bool muse_options_waiting(void) {
    return false;
}

bool muse_options_take(muse_options_t* out) {
    (void)out;
    return false;
}

void muse_options_result(uint32_t generation, muse_options_status_t status,
                                    bool saved, const char* detail) {
    (void)generation;
    (void)status;
    (void)saved;
    (void)detail;
}

bool muse_options_receive(const struct cJSON* payload) {
    (void)payload;
    return false;
}

bool muse_options_receive_message(const struct cJSON* payload, const char* message_id,
                                             const char* streamed_text) {
    (void)payload;
    (void)message_id;
    (void)streamed_text;
    return false;
}

struct cJSON* muse_options_chat_body(const muse_options_t* q,
                                                const char* interaction_id) {
    (void)q;
    (void)interaction_id;
    return nullptr;
}

struct cJSON* muse_options_state_body(const muse_options_t* q,
                                                 const char* mutation_id) {
    (void)q;
    (void)mutation_id;
    return nullptr;
}

bool muse_options_state_response(const muse_options_t* q,
                                            const struct cJSON* response) {
    (void)q;
    (void)response;
    return false;
}

// ---- muse_state: 字幕の折り返し ----
// SDK は「画面の桁数 × 行数」で返答をページングする。SAS-Watch の AI 画面は
// LVGL ラベル (LV_LABEL_LONG_WRAP) でスクロールするので、固定値で十分。
// TODO(hw): 実機の AI 画面の実効幅 (font_body 26px / 410px 幅) に合わせて調整。
void muse_state_page(const char* text, int* cols, int* lines) {
    (void)text;
    if (cols) *cols = 20;
    if (lines) *lines = 8;
}

// ---- SDK ベンチ用の埋め込み MP3 (components/muse/test_reply.mp3) ----
// muse_hatch_mp3_selftest() が参照する。Phase 1 は TTS/音声再生を実装しない
// (minimp3 は sdk/minimp3_shim に置換済み) ので空でよい。
// TODO(muse): TTS を足すときに SDK の MP3 か自前のサンプルに差し替える。
extern "C" const unsigned char _binary_test_reply_mp3_start[1] = {0};
extern "C" const unsigned char _binary_test_reply_mp3_end[1] = {0};

// ---- リンク維持用の参照表 (Phase 1 のみ) ----
// Phase 1 はセッションを開始しないため、何も参照しないとリンカが chat の
// オブジェクトを落とし、Kconfig 有効時のサイズが実際の B-min 構成と一致しない。
// ここで代表的な API のアドレスを保持して「リンクされる状態」を作るだけで、
// 実行はしない (init から volatile 読みするだけ)。
// TODO(muse): Phase 2 でセッション開始を実装したらこの表を消す。
static void* const volatile s_link_keep[] __attribute__((used)) = {
    reinterpret_cast<void*>(muse_hatch_start),
    reinterpret_cast<void*>(muse_hatch_ready),
    reinterpret_cast<void*>(muse_hatch_config_changed),
    reinterpret_cast<void*>(muse_hatch_turn_begin),
    reinterpret_cast<void*>(muse_hatch_turn_audio),
    reinterpret_cast<void*>(muse_hatch_turn_end),
    reinterpret_cast<void*>(muse_hatch_turn_event),
    reinterpret_cast<void*>(muse_hatch_turn_caption),
    reinterpret_cast<void*>(muse_hatch_incoming),
    reinterpret_cast<void*>(muse_hatch_text_turn),
    reinterpret_cast<void*>(muse_hatch_mp3_selftest),
};

extern "C" void muse_link_keep_linked(void) {
    // リンカを騙すための 1 読み。値は使わない。
    (void)s_link_keep[0];
}
