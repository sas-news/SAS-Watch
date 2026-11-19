// T52: muse_console_parse のホストテスト (ESP-IDF 不要)。
// 実行: firmware/components/muse_link/test/run.sh
#include <stdio.h>
#include <string.h>

#include "muse_console_parse.h"

static int g_fail = 0;

static void expect_cmd(const char* line, muse_console_cmd_t want, const char* want_value) {
    muse_console_parsed_t p;
    const bool ok = muse_console_parse(line, &p);
    const bool want_ok = want != MUSE_CONSOLE_NONE;
    const char* got = p.value ? p.value : "";
    // value は行の一部を指す (行末の空白を NUL で切らない)。長さで比較する。
    const bool value_ok = want_value ? (p.value_len == strlen(want_value) &&
                                        memcmp(got, want_value, p.value_len) == 0)
                                     : true;
    if (ok != want_ok || (want_ok && p.cmd != want) || !value_ok) {
        ++g_fail;
        printf("FAIL: \"%s\" -> ok=%d cmd=%d value=\"%.*s\" len=%u (want ok=%d cmd=%d value=\"%s\")\n",
               line, (int)ok, (int)p.cmd, (int)p.value_len, got,
               (unsigned)p.value_len, (int)want_ok, (int)want,
               want_value ? want_value : "");
    }
}

int main(void) {
    expect_cmd("muse", MUSE_CONSOLE_HELP, "");
    expect_cmd("muse help", MUSE_CONSOLE_HELP, "");
    expect_cmd("MUSE STATUS", MUSE_CONSOLE_STATUS, "");
    expect_cmd("  muse status  ", MUSE_CONSOLE_STATUS, "");
    expect_cmd("muse status\n", MUSE_CONSOLE_STATUS, "");
    expect_cmd("muse clear\r", MUSE_CONSOLE_CLEAR, "");
    expect_cmd("muse set host example.invalid", MUSE_CONSOLE_SET_HOST, "example.invalid");
    expect_cmd("muse set vm dummy", MUSE_CONSOLE_SET_VM, "dummy");
    expect_cmd("muse set vm_id legacy-name", MUSE_CONSOLE_SET_VM, "legacy-name");
    expect_cmd("muse set token dummy-token-123", MUSE_CONSOLE_SET_TOKEN, "dummy-token-123");
    // 値なし = その項目を消す (provision 側で空文字=クリア)。
    expect_cmd("muse set token", MUSE_CONSOLE_SET_TOKEN, "");
    // 前後の空白は落とす。
    expect_cmd("muse set token   spaced-out   ", MUSE_CONSOLE_SET_TOKEN, "spaced-out");
    // パスワード的な文字列中の空白は parse では許容 (validate は provision 側)。
    expect_cmd("muse set host a b", MUSE_CONSOLE_SET_HOST, "a b");

    // Wi-Fi 投入コマンド (T53/M1: 空白を含む SSID/パスワード可)。
    expect_cmd("muse set ssid My Home AP", MUSE_CONSOLE_SET_SSID, "My Home AP");
    expect_cmd("muse set pass secret pass 1", MUSE_CONSOLE_SET_PASS, "secret pass 1");
    expect_cmd("muse set password pw-123", MUSE_CONSOLE_SET_PASS, "pw-123");
    expect_cmd("muse set pass", MUSE_CONSOLE_SET_PASS, "");

    // T55: セッション操作
    expect_cmd("muse start", MUSE_CONSOLE_START, "");
    expect_cmd("MUSE STOP", MUSE_CONSOLE_STOP, "");
    expect_cmd("muse reply", MUSE_CONSOLE_REPLY, "");
    expect_cmd("muse reply now", MUSE_CONSOLE_NONE, NULL);
    expect_cmd("muse send hello world", MUSE_CONSOLE_SEND, "hello world");
    expect_cmd("muse send   padded  ", MUSE_CONSOLE_SEND, "padded");
    expect_cmd("muse send", MUSE_CONSOLE_SEND, "");
    expect_cmd("muse sends", MUSE_CONSOLE_NONE, NULL);

    // 解釈できない行。
    expect_cmd("muse set", MUSE_CONSOLE_NONE, NULL);
    expect_cmd("muse set foo bar", MUSE_CONSOLE_NONE, NULL);
    expect_cmd("status", MUSE_CONSOLE_NONE, NULL);
    expect_cmd("muses status", MUSE_CONSOLE_NONE, NULL);
    expect_cmd("muse statusx", MUSE_CONSOLE_NONE, NULL);
    expect_cmd("muse set hostExample", MUSE_CONSOLE_NONE, NULL);
    expect_cmd("", MUSE_CONSOLE_NONE, NULL);

    // 1023B の token (SDK の MUSE_TOKEN_MAX) が値を欠かさず 1 行で扱えること。
    {
        char line[1200];
        int n = snprintf(line, sizeof(line), "muse set token ");
        for (int i = 0; i < 1023; ++i) {
            line[n++] = (char)('a' + (i % 26));
        }
        line[n] = '\0';
        muse_console_parsed_t p;
        if (!muse_console_parse(line, &p) || p.cmd != MUSE_CONSOLE_SET_TOKEN ||
            p.value_len != 1023) {
            ++g_fail;
            printf("FAIL: 1023-byte token (ok=%d cmd=%d len=%u)\n",
                   (int)muse_console_parse(line, &p), (int)p.cmd, (unsigned)p.value_len);
        }
    }

    if (g_fail == 0) {
        printf("test_muse_console: all passed\n");
        return 0;
    }
    printf("test_muse_console: %d failure(s)\n", g_fail);
    return 1;
}
