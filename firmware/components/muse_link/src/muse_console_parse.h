// muse_console_parse — シリアル `muse ...` 行のパーサ (T52)。
//
// ESP-IDF に依存しない純 C。ホストのテスト (test/test_muse_console.c) から
// そのままコンパイルできるようにしている。
//
// 受け付ける形 (大文字小文字はコマンド語のみ無視):
//   muse                           -> HELP
//   muse help                      -> HELP
//   muse status                    -> STATUS
//   muse clear                     -> CLEAR
//   muse set host <value>          -> SET_HOST
//   muse set vm <value>            -> SET_VM   (vm_id も可)
//   muse set token <value>         -> SET_TOKEN
//   muse start | muse stop         -> START / STOP
//   muse send <text>               -> SEND (value = text)
//   muse reply                     -> REPLY
//
// value は前後の空白を除いた残り。空 (例 "muse set token") は「その項目を消す」
// として扱う (呼び出し側の muse_link_provision が空文字=クリア)。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MUSE_CONSOLE_NONE = 0,
    MUSE_CONSOLE_HELP,
    MUSE_CONSOLE_STATUS,
    MUSE_CONSOLE_CLEAR,
    MUSE_CONSOLE_SET_HOST,
    MUSE_CONSOLE_SET_VM,
    MUSE_CONSOLE_SET_TOKEN,
    MUSE_CONSOLE_SET_SSID,
    MUSE_CONSOLE_SET_PASS,
    // T55 Phase 2 ③ (セッション)
    MUSE_CONSOLE_START,
    MUSE_CONSOLE_STOP,
    MUSE_CONSOLE_SEND,   // value = 送るテキスト
    MUSE_CONSOLE_REPLY,  // 最新の返答の長さだけ表示
} muse_console_cmd_t;

typedef struct {
    muse_console_cmd_t cmd;
    const char* value;  // 入力行の一部を指す (書き換え不可)。値なしなら ""
    size_t value_len;
} muse_console_parsed_t;

// 1 行を解析する。`muse` コマンドとして解釈できたら true。
// 解釈できない行 (別コマンド・未知のサブコマンド) は false + cmd=NONE。
// line は NUL 終端。CR/LF は無視する。
bool muse_console_parse(const char* line, muse_console_parsed_t* out);

#ifdef __cplusplus
}
#endif
