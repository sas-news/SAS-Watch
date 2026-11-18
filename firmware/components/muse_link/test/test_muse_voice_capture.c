// T57: 音声入力 (15 秒の音声ノート) の純 C 状態機械のホストテスト。
// 実行: firmware/components/muse_link/test/run.sh
#include <stdio.h>
#include <string.h>

#include "muse_link/muse_link.h"
#include "muse_voice_logic.h"

static int g_fail = 0;

#define CHECK(cond, ...)                  \
    do {                                  \
        if (!(cond)) {                    \
            ++g_fail;                     \
            printf("FAIL: " __VA_ARGS__); \
            printf("\n");                 \
        }                                 \
    } while (0)

static void test_begin(void)
{
    muse_voice_logic_t l;
    muse_voice_logic_init(&l);
    CHECK(l.state == MUSE_LINK_VOICE_IDLE, "init -> idle");
    CHECK(l.error[0] == '\0', "init no error");

    // max_sec は 1..15 にクランプされる (Muse 側の打ち切り)。
    CHECK(muse_voice_logic_begin(&l, 99, 1000), "begin clamps max");
    CHECK(l.state == MUSE_LINK_VOICE_RECORDING, "recording");
    CHECK(l.max_sec == MUSE_VOICE_HARD_MAX_SEC, "max clamped to 15");
    CHECK(l.max_samples == 15u * MUSE_VOICE_SAMPLE_RATE, "capacity = 15s samples");
    CHECK(l.started_ms == 1000, "started_ms");

    // 録音中の二重開始は不可。
    CHECK(!muse_voice_logic_begin(&l, 5, 2000), "double begin rejected");

    muse_voice_logic_reset(&l);
    CHECK(muse_voice_logic_begin(&l, 0, 0), "begin min clamp");
    CHECK(l.max_sec == 1, "max clamped to 1");
}

static void test_append(void)
{
    muse_voice_logic_t l;
    muse_voice_logic_init(&l);
    CHECK(muse_voice_logic_begin(&l, 15, 0), "begin");

    CHECK(muse_voice_logic_append(&l, 16000), "append 1s");
    CHECK(muse_voice_logic_append(&l, 16000 * 14), "append 14s");
    CHECK(l.samples == 15u * MUSE_VOICE_SAMPLE_RATE, "samples = 15s");

    // 容量超過は false + 日本語エラー。
    CHECK(!muse_voice_logic_append(&l, 1), "overflow rejected");
    CHECK(strstr(l.error, "15 秒") != NULL, "overflow message");
    CHECK(l.samples == 15u * MUSE_VOICE_SAMPLE_RATE, "samples unchanged");

    // 0 サンプルは無視 (false)。
    muse_voice_logic_reset(&l);
    CHECK(muse_voice_logic_begin(&l, 15, 0), "begin again");
    CHECK(!muse_voice_logic_append(&l, 0), "zero append rejected");

    // 録音していないときは append 不可。
    muse_voice_logic_reset(&l);
    CHECK(!muse_voice_logic_append(&l, 160), "append while idle rejected");
}

static void test_elapsed_and_auto_stop(void)
{
    muse_voice_logic_t l;
    muse_voice_logic_init(&l);
    CHECK(muse_voice_logic_begin(&l, 15, 1000), "begin");
    muse_voice_logic_tick(&l, 1000);
    CHECK(l.elapsed_s == 0, "elapsed 0");
    CHECK(!muse_voice_logic_should_auto_stop(&l), "not auto stop yet");

    muse_voice_logic_tick(&l, 1000 + 14999);
    CHECK(l.elapsed_s == 14, "elapsed 14");
    CHECK(!muse_voice_logic_should_auto_stop(&l), "still not 15");

    muse_voice_logic_tick(&l, 1000 + 15000);
    CHECK(l.elapsed_s == 15, "elapsed 15");
    CHECK(muse_voice_logic_should_auto_stop(&l), "auto stop at 15s");

    // 蛇行しても最大 15 で頭打ち。
    muse_voice_logic_tick(&l, 1000 + 60000);
    CHECK(l.elapsed_s == 15, "elapsed clamped");

    // tick は録音中だけ効く。
    muse_voice_logic_mark_sending(&l);
    muse_voice_logic_tick(&l, 1000 + 20000);
    CHECK(l.elapsed_s == 0, "no elapsed while sending");
}

static void test_states_and_errors(void)
{
    muse_voice_logic_t l;
    muse_voice_logic_init(&l);
    CHECK(strcmp(muse_voice_state_name(MUSE_LINK_VOICE_IDLE), "idle") == 0, "idle name");
    CHECK(strcmp(muse_voice_state_name(MUSE_LINK_VOICE_RECORDING), "recording") == 0,
          "recording name");
    CHECK(strcmp(muse_voice_state_name(MUSE_LINK_VOICE_SENDING), "sending") == 0,
          "sending name");
    CHECK(strcmp(muse_voice_state_name(MUSE_LINK_VOICE_ERROR), "error") == 0, "error name");

    CHECK(muse_voice_logic_begin(&l, 15, 0), "begin");
    muse_voice_logic_append(&l, 320);
    muse_voice_logic_mark_sending(&l);
    CHECK(l.state == MUSE_LINK_VOICE_SENDING, "sending");
    CHECK(muse_voice_logic_begin(&l, 15, 0) == false, "begin while sending rejected");

    muse_voice_logic_mark_error(&l, "PSRAM が不足しています（テーマを軽くしてください）");
    CHECK(l.state == MUSE_LINK_VOICE_ERROR, "error");
    CHECK(strstr(l.error, "PSRAM") != NULL, "error message");

    // エラー後は再試行できる。
    muse_voice_logic_reset(&l);
    CHECK(l.state == MUSE_LINK_VOICE_IDLE, "reset -> idle");
    CHECK(l.error[0] == '\0', "reset clears error");
    CHECK(muse_voice_logic_begin(&l, 15, 0), "retry after error");
}

int main(void)
{
    test_begin();
    test_append();
    test_elapsed_and_auto_stop();
    test_states_and_errors();
    if (g_fail > 0) {
        printf("%d voice capture test(s) FAILED\n", g_fail);
        return 1;
    }
    printf("all voice capture tests passed\n");
    return 0;
}
