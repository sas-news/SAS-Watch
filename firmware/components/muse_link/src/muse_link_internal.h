// muse_link 内部共有 (muse_link.cpp <-> muse_adapters.cpp)。
// 公開 API ではない。SDK の muse_settings_* と同じシグネチャで NVS キャッシュを返す。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "muse_settings.h"  // MUSE_HOST_MAX / MUSE_VM_MAX / MUSE_TOKEN_MAX

#ifdef __cplusplus
extern "C" {
#endif

// muse_link_init() が読んだ値を返す。未初期化なら既定値 (host は既定ホスト、他は空)。
void muse_link_cfg_host(char out[MUSE_HOST_MAX + 1]);
void muse_link_cfg_vm(char out[MUSE_VM_MAX + 1]);
void muse_link_cfg_token(char out[MUSE_TOKEN_MAX + 1]);
size_t muse_link_cfg_token_len(void);

#ifdef __cplusplus
}
#endif
