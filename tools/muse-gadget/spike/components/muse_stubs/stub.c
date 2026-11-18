/*
 * T49 spike: SDK の components/muse から「chat に必要な関数」だけをスタブする。
 *
 * SDK 本体では muse_settings.c / muse_link.c / muse_options.c / muse_state.c /
 * muse_wifi.c (main/wifi_mgr.c) が実装している。B-min で SAS-Watch に移植するときは
 * これらを SAS-Watch の設定 (NVS + core Settings) / BLE / wifi コンポーネントに
 * 置き換える。ここでは「chat/Noise 層が 5.5.1 でコンパイル・リンクできるか」だけを
 * 見るため、値は固定 (未接続・未設定) にしてある。
 *
 * 返り値の意味は SDK ヘッダのコメントどおり。動作はしない。
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "muse_link.h"
#include "muse_options.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_wifi.h"

/* ---- muse_settings: 設定 (NVS) ---- */

void muse_settings_hatch_host(char out[MUSE_HOST_MAX + 1])
{
    strncpy(out, "hatch.metaaivm.com", MUSE_HOST_MAX);
    out[MUSE_HOST_MAX] = '\0';
}

void muse_settings_hatch_vm(char out[MUSE_VM_MAX + 1])
{
    out[0] = '\0';
}

void muse_settings_hatch_token(char out[MUSE_TOKEN_MAX + 1])
{
    out[0] = '\0';
}

size_t muse_settings_hatch_token_len(void)
{
    return 0;
}

/* ---- muse_link: Home Link のセッション/アカウント ---- */

bool muse_link_hatch_linked(void)
{
    return false;
}

bool muse_link_hatch_vm(const char *want_vm, char *vm_id, size_t id_cap, char *vm_name,
                        size_t name_cap, char **vm_token)
{
    (void)want_vm;
    (void)id_cap;
    (void)name_cap;
    if (vm_id[0]) vm_id[0] = '\0';
    if (vm_name[0]) vm_name[0] = '\0';
    if (vm_token) *vm_token = NULL;
    return false;
}

/* ---- muse_wifi: Wi-Fi 状態 ---- */

bool muse_wifi_connected(void)
{
    return false;
}

/* ---- muse_options: 選択肢 UI (B-min では使わない) ---- */

bool muse_options_snapshot(muse_options_t *out)
{
    (void)out;
    return false;
}

void muse_options_clear(void) {}

void muse_options_disconnected(void) {}

bool muse_options_pending(void)
{
    return false;
}

bool muse_options_waiting(void)
{
    return false;
}

bool muse_options_take(muse_options_t *out)
{
    (void)out;
    return false;
}

void muse_options_result(uint32_t generation, muse_options_status_t status, bool saved,
                         const char *detail)
{
    (void)generation;
    (void)status;
    (void)saved;
    (void)detail;
}

bool muse_options_receive(const struct cJSON *payload)
{
    (void)payload;
    return false;
}

bool muse_options_receive_message(const struct cJSON *payload, const char *message_id,
                                  const char *streamed_text)
{
    (void)payload;
    (void)message_id;
    (void)streamed_text;
    return false;
}

struct cJSON *muse_options_chat_body(const muse_options_t *q, const char *interaction_id)
{
    (void)q;
    (void)interaction_id;
    return NULL;
}

struct cJSON *muse_options_state_body(const muse_options_t *q, const char *mutation_id)
{
    (void)q;
    (void)mutation_id;
    return NULL;
}

bool muse_options_state_response(const muse_options_t *q, const struct cJSON *response)
{
    (void)q;
    (void)response;
    return false;
}

/* ---- muse_state: 字幕のページ計算 (B-min では SAS-Watch の UI 幅を返す想定) ---- */

void muse_state_page(const char *text, int *cols, int *lines)
{
    (void)text;
    if (cols) *cols = 20;
    if (lines) *lines = 8;
}

/* ---- SDK bench asset: components/muse/test_reply.mp3 ----
 * muse_hatch_mp3_selftest() が参照する埋め込み MP3 のシンボル。
 * B-min では TTS を実装しないので、ベンチ用のこの 1 関数だけ空データで足りる
 * (クラス (b): スタブで回避)。*/
const unsigned char _binary_test_reply_mp3_start[1] = {0};
const unsigned char _binary_test_reply_mp3_end[1] = {0};
