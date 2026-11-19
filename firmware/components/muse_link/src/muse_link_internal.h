// muse_link 内部共有 (muse_link.cpp <-> muse_adapters.cpp)。
// 公開 API ではない。SDK の muse_settings_* と同じシグネチャで NVS キャッシュを返す。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "muse_settings.h"  // MUSE_HOST_MAX / MUSE_VM_MAX / MUSE_TOKEN_MAX

#include "muse_link/muse_link.h"
#include "session_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

// muse_link_init() が読んだ値を返す。未初期化なら既定値 (host は既定ホスト、他は空)。
void muse_link_cfg_host(char out[MUSE_HOST_MAX + 1]);
void muse_link_cfg_vm(char out[MUSE_VM_MAX + 1]);
void muse_link_cfg_token(char out[MUSE_TOKEN_MAX + 1]);
size_t muse_link_cfg_token_len(void);

// muse_link.cpp の NVS キャッシュを、セッション側の status 組み立てに渡す。
void muse_link_cfg_get(char* host, size_t host_cap, char* vm, size_t vm_cap,
                       size_t* token_len);

// muse_session.cpp: セッション由来の status フィールドを埋める。
void muse_link_session_fill_status(muse_link_status_fields_t* f);

// muse_session.cpp: PSRAM 上の PCM バッファ所有権を引き取って音声ノートを送信する。
// true を返すと muse_session 側が送信完了/中止時に heap_caps_free() する。
// false を返すと所有権は呼び出し側に残る (呼び出し側で free すること)。
bool muse_link_take_voice_note(int16_t* pcm_psram, size_t samples);

#ifdef __cplusplus
}
#endif
