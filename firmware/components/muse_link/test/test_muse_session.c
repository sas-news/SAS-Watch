// T55: セッション状態機械 / 開始前チェック / status JSON のホストテスト。
// 実行: firmware/components/muse_link/test/run.sh
#include <stdio.h>
#include <string.h>

#include "session_logic.h"

static int g_fail = 0;

#define CHECK(cond, ...)                  \
    do {                                  \
        if (!(cond)) {                    \
            ++g_fail;                     \
            printf("FAIL: " __VA_ARGS__); \
            printf("\n");                 \
        }                                 \
    } while (0)

static void expect_advance(muse_link_session_state_t from, muse_session_event_t ev,
                           muse_link_session_state_t want, bool want_changed)
{
    bool changed = false;
    const muse_link_session_state_t got = muse_link_session_advance(from, ev, &changed);
    CHECK(got == want && changed == want_changed,
          "advance(%d, ev=%d) -> state=%d changed=%d (want %d/%d)", (int)from, (int)ev,
          (int)got, (int)changed, (int)want, (int)want_changed);
}

static void test_fsm(void)
{
    const muse_link_session_state_t OFF = MUSE_LINK_SESSION_OFF;
    const muse_link_session_state_t STARTING = MUSE_LINK_SESSION_STARTING;
    const muse_link_session_state_t READY = MUSE_LINK_SESSION_READY;
    const muse_link_session_state_t BUSY = MUSE_LINK_SESSION_BUSY;
    const muse_link_session_state_t ERROR = MUSE_LINK_SESSION_ERROR;

    // 起動
    expect_advance(OFF, MUSE_SESSION_EV_START, STARTING, true);
    expect_advance(ERROR, MUSE_SESSION_EV_START, STARTING, true);
    expect_advance(STARTING, MUSE_SESSION_EV_START, STARTING, false);
    expect_advance(READY, MUSE_SESSION_EV_START, READY, false);
    expect_advance(BUSY, MUSE_SESSION_EV_START, BUSY, false);

    // Wi-Fi 失敗 → error
    expect_advance(STARTING, MUSE_SESSION_EV_WIFI_FAIL, ERROR, true);
    expect_advance(STARTING, MUSE_SESSION_EV_WIFI_OK, STARTING, false);
    expect_advance(READY, MUSE_SESSION_EV_WIFI_FAIL, READY, false);

    // 接続/切断
    expect_advance(STARTING, MUSE_SESSION_EV_CONNECTED, READY, true);
    expect_advance(BUSY, MUSE_SESSION_EV_CONNECTED, BUSY, false);
    expect_advance(READY, MUSE_SESSION_EV_CONNECTED, READY, false);
    expect_advance(READY, MUSE_SESSION_EV_DISCONNECTED, ERROR, true);
    expect_advance(BUSY, MUSE_SESSION_EV_DISCONNECTED, ERROR, true);
    expect_advance(STARTING, MUSE_SESSION_EV_DISCONNECTED, ERROR, true);
    expect_advance(OFF, MUSE_SESSION_EV_DISCONNECTED, OFF, false);

    // ターン
    expect_advance(READY, MUSE_SESSION_EV_TURN_BEGIN, BUSY, true);
    expect_advance(BUSY, MUSE_SESSION_EV_TURN_BEGIN, BUSY, false);
    expect_advance(OFF, MUSE_SESSION_EV_TURN_BEGIN, OFF, false);  // 未起動では動かない
    expect_advance(BUSY, MUSE_SESSION_EV_TURN_DONE, READY, true);
    expect_advance(READY, MUSE_SESSION_EV_TURN_DONE, READY, false);
    expect_advance(BUSY, MUSE_SESSION_EV_TURN_ERROR, ERROR, true);
    expect_advance(READY, MUSE_SESSION_EV_TURN_ERROR, ERROR, true);

    // 停止はどこからでも OFF
    expect_advance(OFF, MUSE_SESSION_EV_STOP, OFF, false);
    expect_advance(STARTING, MUSE_SESSION_EV_STOP, OFF, true);
    expect_advance(READY, MUSE_SESSION_EV_STOP, OFF, true);
    expect_advance(BUSY, MUSE_SESSION_EV_STOP, OFF, true);
    expect_advance(ERROR, MUSE_SESSION_EV_STOP, OFF, true);
}

static void test_start_check(void)
{
    // 正常
    CHECK(muse_link_session_check_start(true, true, 20000, 2000000, 12288, 1433600) ==
              MUSE_LINK_START_OK,
          "check_start ok");
    // 資格情報なし
    CHECK(muse_link_session_check_start(false, true, 20000, 2000000, 12288, 1433600) ==
              MUSE_LINK_START_ERR_NOT_CONFIGURED,
          "not configured");
    // Wi-Fi 未設定
    CHECK(muse_link_session_check_start(true, false, 20000, 2000000, 12288, 1433600) ==
              MUSE_LINK_START_ERR_WIFI_NOT_CONFIGURED,
          "wifi not configured");
    // 内蔵 SRAM 不足
    CHECK(muse_link_session_check_start(true, true, 8000, 2000000, 12288, 1433600) ==
              MUSE_LINK_START_ERR_NO_INTERNAL_RAM,
          "no internal ram");
    // PSRAM 不足 (cute/cyber テーマの残り ~750KB を想定)
    CHECK(muse_link_session_check_start(true, true, 20000, 750000, 12288, 1433600) ==
              MUSE_LINK_START_ERR_NO_PSRAM,
          "no psram");
    // 0 = チェック無効
    CHECK(muse_link_session_check_start(true, true, 1, 1, 0, 0) == MUSE_LINK_START_OK,
          "threshold 0 disables checks");

    // エラー文言 (日本語・秘密なし)
    CHECK(strstr(muse_link_session_error_text(MUSE_LINK_START_ERR_NOT_CONFIGURED),
                 "資格情報") != NULL,
          "not-configured message is Japanese");
    CHECK(strstr(muse_link_session_error_text(MUSE_LINK_START_ERR_NO_INTERNAL_RAM),
                 "内蔵メモリ") != NULL,
          "internal-ram message");
    CHECK(strstr(muse_link_session_error_text(MUSE_LINK_START_ERR_NO_PSRAM), "PSRAM") != NULL,
          "psram message");
    CHECK(strstr(muse_link_session_error_text(MUSE_LINK_START_ERR_BUSY), "起動") != NULL,
          "busy message");
    CHECK(strlen(muse_link_session_error_text(MUSE_LINK_START_OK)) == 0, "ok message empty");
}

static void test_status_json(void)
{
    char buf[512];
    muse_link_status_fields_t f = {};
    f.configured = true;
    f.ready = true;
    f.wifi = true;
    f.wifi_owned = true;
    f.wifi_state = 2;  // connected
    f.session = MUSE_LINK_SESSION_BUSY;
    f.last_reply_len = 12;
    f.total_reply_len = 4096;
    f.host = "hatch.metaaivm.com";
    f.vm = "vm-1";
    f.token_len = 42;
    f.last_error = "";

    CHECK(muse_link_session_status_json(buf, sizeof(buf), &f), "status json builds");
    CHECK(strstr(buf, "\"session\":\"busy\"") != NULL, "session state name");
    CHECK(strstr(buf, "\"wifi_state\":\"connected\"") != NULL, "wifi state name");
    CHECK(strstr(buf, "\"last_reply_len\":12") != NULL, "last reply len");
    CHECK(strstr(buf, "\"total_reply_len\":4096") != NULL, "total reply len");
    CHECK(strstr(buf, "\"token_len\":42") != NULL, "token len");
    CHECK(strstr(buf, "\"host\":\"hatch.metaaivm.com\"") != NULL, "host");
    // token の値そのものは入らない (長さだけ)。
    CHECK(strstr(buf, "token\":\"") == NULL, "no token value field");

    // エラー文言・JSON エスケープ
    f.session = MUSE_LINK_SESSION_ERROR;
    f.last_error = "quote\" and\\back";
    CHECK(muse_link_session_status_json(buf, sizeof(buf), &f), "error json builds");
    CHECK(strstr(buf, "\"session\":\"error\"") != NULL, "error state");
    CHECK(strstr(buf, "quote\\\" and\\\\back") != NULL, "error escaped");

    // バッファ不足
    CHECK(!muse_link_session_status_json(buf, 16, &f), "small buffer fails");

    // 状態名
    CHECK(strcmp(muse_link_session_state_name(MUSE_LINK_SESSION_OFF), "off") == 0, "off name");
    CHECK(strcmp(muse_link_session_state_name(MUSE_LINK_SESSION_STARTING), "starting") == 0,
          "starting name");
    CHECK(strcmp(muse_link_session_state_name(MUSE_LINK_SESSION_READY), "ready") == 0,
          "ready name");
    CHECK(strcmp(muse_link_session_state_name(MUSE_LINK_SESSION_BUSY), "busy") == 0, "busy name");
    CHECK(strcmp(muse_link_session_state_name(MUSE_LINK_SESSION_ERROR), "error") == 0,
          "error name");
}

int main(void)
{
    test_fsm();
    test_start_check();
    test_status_json();

    if (g_fail == 0) {
        printf("test_muse_session: all passed\n");
        return 0;
    }
    printf("test_muse_session: %d failure(s)\n", g_fail);
    return 1;
}
