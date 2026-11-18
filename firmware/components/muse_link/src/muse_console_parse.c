// muse_console_parse — シリアル `muse ...` 行のパーサ (T52)。
// 純 C / ESP-IDF 非依存 (ホストテストから使う)。実装メモは
// docs/muse-bmin.md の「Phase 2 ①」を参照。
#include "muse_console_parse.h"

#include <ctype.h>
#include <string.h>

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static const char *skip_space(const char *p)
{
    while (*p && is_space(*p)) {
        ++p;
    }
    return p;
}

// p から語を 1 つ取り出す (英数字と '_' のみ)。大文字小文字は無視して比較する。
static bool take_word(const char **p, const char *word)
{
    const char *s = *p;
    const char *w = word;
    while (*w) {
        if (!*s || tolower((unsigned char)*s) != tolower((unsigned char)*w)) {
            return false;
        }
        ++s;
        ++w;
    }
    // 語の直後は空白か終端であること ("statusx" を status と誤認しない)。
    if (*s && !is_space(*s)) {
        return false;
    }
    *p = s;
    return true;
}

bool muse_console_parse(const char *line, muse_console_parsed_t *out)
{
    const char *p;
    const char *v;
    const char *end;
    muse_console_cmd_t cmd;
    const char *after_field;

    if (!out) {
        return false;
    }
    out->cmd = MUSE_CONSOLE_NONE;
    out->value = "";
    out->value_len = 0;
    if (!line) {
        return false;
    }

    p = skip_space(line);
    if (!take_word(&p, "muse")) {
        return false;
    }
    p = skip_space(p);

    if (!*p) {
        out->cmd = MUSE_CONSOLE_HELP;
        return true;
    }
    // 引数を取らないコマンドは、語の後に余計な文字が無いこと。
    if (take_word(&p, "help")) {
        if (*skip_space(p)) return false;
        out->cmd = MUSE_CONSOLE_HELP;
        return true;
    }
    if (take_word(&p, "status")) {
        if (*skip_space(p)) return false;
        out->cmd = MUSE_CONSOLE_STATUS;
        return true;
    }
    if (take_word(&p, "clear")) {
        if (*skip_space(p)) return false;
        out->cmd = MUSE_CONSOLE_CLEAR;
        return true;
    }
    if (take_word(&p, "start")) {
        if (*skip_space(p)) return false;
        out->cmd = MUSE_CONSOLE_START;
        return true;
    }
    if (take_word(&p, "stop")) {
        if (*skip_space(p)) return false;
        out->cmd = MUSE_CONSOLE_STOP;
        return true;
    }
    if (take_word(&p, "reply")) {
        if (*skip_space(p)) return false;
        out->cmd = MUSE_CONSOLE_REPLY;
        return true;
    }
    if (take_word(&p, "send")) {
        // 残り全部が本文 (前後の空白だけ落とす)。
        v = skip_space(p);
        end = v + strlen(v);
        while (end > v && is_space(end[-1])) {
            --end;
        }
        out->cmd = MUSE_CONSOLE_SEND;
        out->value = v;
        out->value_len = (size_t)(end - v);
        return true;
    }
    if (!take_word(&p, "set")) {
        return false;
    }
    p = skip_space(p);

    cmd = MUSE_CONSOLE_NONE;
    after_field = p;
    if (take_word(&after_field, "host")) {
        cmd = MUSE_CONSOLE_SET_HOST;
    } else if (take_word(&after_field, "vm") || take_word(&after_field, "vm_id")) {
        cmd = MUSE_CONSOLE_SET_VM;
    } else if (take_word(&after_field, "token")) {
        cmd = MUSE_CONSOLE_SET_TOKEN;
    } else {
        return false;
    }

    v = skip_space(after_field);
    end = v + strlen(v);
    while (end > v && is_space(end[-1])) {
        --end;
    }
    out->cmd = cmd;
    out->value = v;
    out->value_len = (size_t)(end - v);
    return true;
}
