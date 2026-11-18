// test_muse.cpp — Muse アプリ (Phase 2 ⑤ UI 字幕) のホストテスト。
// Fake MusePort で状態写像・エラー・返答のページ分割 (UTF-8 境界)・
// 15 秒の自動ページ送り・画面が見えている間だけのポーリングを検査する。
// apps.cmake が SAS_APP_TEST_SOURCES として core のテストに取り込む。
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "fakes.hpp"
#include "watch/features/muse.hpp"
#include "watch/navigation.hpp"
#include "watch/power.hpp"
#include "watch/settings.hpp"

using watch::Action;
using watch::ActionType;
using watch::EventBus;
using watch::FeatureContext;
using watch::MuseState;
using watch::MuseVoiceState;
using watch::Navigator;
using watch::PowerPolicy;
using watch::Route;
using watch::Settings;
using watch::features::kMuse;
using watch::features::kMusePageBytes;
using watch::features::kMusePageMs;
using watch::features::kMuseReplyMax;

namespace {

struct MuseFixture {
  EventBus bus;
  watch::test::MemoryKeyValueStore kv;
  watch::test::FakeClock clock;
  Settings settings;
  Navigator nav{&bus};
  PowerPolicy power{&bus};
  watch::test::FakeMuse muse;
  FeatureContext ctx{bus, kv, clock, &nav, &power};

  MuseFixture() {
    ctx.settings = &settings;
    ctx.muse = &muse;
    watch::features::muse_reset_state();
    nav.push(Route::App1);  // Muse 画面 = 現在の画面
  }

  void tick(int64_t now) { kMuse.tick(now, ctx); }

  bool act(ActionType t) {
    Action a{};
    a.type = t;
    return kMuse.handle(a, ctx);
  }

  // 現在ページの本文 (空なら "")。
  std::string page() {
    char buf[watch::features::kMusePageBytes + 8];
    if (!watch::features::muse_page_text(buf, sizeof(buf))) return {};
    return buf;
  }

  // 全ページを連結する (返答の切り詰め/UTF-8 境界の検査用)。
  // 途中のページから始まっていても先頭に戻してから連結する。
  std::string all_pages() {
    if (watch::features::muse_pages() == 0) return {};
    while (watch::features::muse_page() != 0) {
      watch::features::muse_next_page();
    }
    std::string out;
    for (uint16_t i = 0; i < watch::features::muse_pages(); ++i) {
      out += page();
      watch::features::muse_next_page();
    }
    return out;
  }
};

// 文字の途中で切れていない UTF-8 か。
bool valid_utf8(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    const uint8_t c = static_cast<uint8_t>(s[i]);
    const size_t n = c < 0x80   ? 1
                     : (c & 0xE0) == 0xC0 ? 2
                     : (c & 0xF0) == 0xE0 ? 3
                     : (c & 0xF8) == 0xF0 ? 4
                                          : 1;
    if (i + n > s.size()) return false;
    i += n;
  }
  return true;
}

}  // namespace

// ---- 無効 / 未設定 ----

TEST(Muse, PortNullptrShowsUnavailable) {
  MuseFixture f;
  f.ctx.muse = nullptr;  // 既定 off ファーム (CONFIG_WATCH_ENABLE_MUSE_LINK=n)
  f.tick(1000);
  EXPECT_FALSE(watch::features::muse_available());
  EXPECT_EQ(watch::features::muse_state(), MuseState::Unavailable);
  EXPECT_EQ(watch::features::muse_pages(), 0u);
  EXPECT_FALSE(watch::features::muse_page_text(nullptr, 0));
  // 状態が変わらないのでポーリングしない (電池優先)。
  EXPECT_EQ(kMuse.next_deadline_ms(), 0);
}

TEST(Muse, PortUnavailableShowsUnavailable) {
  MuseFixture f;
  f.muse.available_ = false;
  f.muse.set_state(MuseState::Ready);
  f.tick(1000);
  EXPECT_FALSE(watch::features::muse_available());
  EXPECT_EQ(watch::features::muse_state(), MuseState::Unavailable);
}

TEST(Muse, UnconfiguredStateIsMapped) {
  MuseFixture f;
  f.muse.set_state(MuseState::Unconfigured);
  f.tick(1000);
  EXPECT_TRUE(watch::features::muse_available());
  EXPECT_EQ(watch::features::muse_state(), MuseState::Unconfigured);
  // 未設定では接続操作をしない (資格情報の投入は別経路)。
  f.act(ActionType::PrimaryAction);
  EXPECT_EQ(f.muse.start_calls, 0);
}

TEST(Muse, ErrorTextIsJapaneseFromPort) {
  MuseFixture f;
  f.muse.set_state(MuseState::Error);
  f.muse.set_error("Wi-Fi に接続できませんでした");
  f.tick(1000);
  EXPECT_EQ(watch::features::muse_state(), MuseState::Error);
  EXPECT_STREQ(watch::features::muse_error(), "Wi-Fi に接続できませんでした");
}

// ---- 接続/切断 (PrimaryAction / LongPress) ----

TEST(Muse, PrimaryActionTogglesStartAndStop) {
  MuseFixture f;
  f.muse.set_state(MuseState::Off);
  f.tick(1000);  // Ready な snapshot にする
  EXPECT_TRUE(f.act(ActionType::PrimaryAction));
  EXPECT_EQ(f.muse.start_calls, 1);
  EXPECT_EQ(f.muse.stop_calls, 0);
  f.tick(2000);
  EXPECT_EQ(watch::features::muse_state(), MuseState::Starting);
  // 接続中にもう一度押すと切断。
  EXPECT_TRUE(f.act(ActionType::PrimaryAction));
  EXPECT_EQ(f.muse.stop_calls, 1);
  f.tick(3000);
  EXPECT_EQ(watch::features::muse_state(), MuseState::Off);
}

TEST(Muse, LongPressStops) {
  MuseFixture f;
  f.muse.set_state(MuseState::Ready);
  f.tick(1000);
  EXPECT_TRUE(f.act(ActionType::LongPress));
  EXPECT_EQ(f.muse.stop_calls, 1);
}

TEST(Muse, SendTextGoesToPort) {
  MuseFixture f;
  f.muse.set_state(MuseState::Ready);
  f.tick(1000);
  EXPECT_TRUE(watch::features::muse_request_send_text(f.ctx, "こんにちは"));
  EXPECT_EQ(f.muse.send_calls, 1);
  EXPECT_EQ(f.muse.last_sent, "こんにちは");
  EXPECT_FALSE(watch::features::muse_request_send_text(f.ctx, ""));
}

// ---- 返答のページ分割 ----

TEST(Muse, ReplyIsSplitIntoPages) {
  MuseFixture f;
  f.muse.set_state(MuseState::Ready);
  std::string reply(700, 'a');  // 300 + 300 + 100 = 3 ページ
  f.muse.set_reply(reply.c_str());
  f.tick(1000);
  EXPECT_EQ(watch::features::muse_pages(), 3u);
  EXPECT_EQ(watch::features::muse_page(), 0u);
  EXPECT_EQ(f.page().size(), kMusePageBytes);
  watch::features::muse_next_page();
  EXPECT_EQ(watch::features::muse_page(), 1u);
  EXPECT_EQ(f.page().size(), kMusePageBytes);
  watch::features::muse_next_page();
  EXPECT_EQ(watch::features::muse_page(), 2u);
  EXPECT_EQ(f.page().size(), 100u);
  // 最後の次は先頭へ戻る。
  watch::features::muse_next_page();
  EXPECT_EQ(watch::features::muse_page(), 0u);
  EXPECT_EQ(f.all_pages(), reply);
}

TEST(Muse, PageSplitKeepsUtf8Boundary) {
  MuseFixture f;
  f.muse.set_state(MuseState::Ready);
  // 299B の ASCII + 3B の日本語 (302B)。300B で切ると「あ」の途中になるので、
  // 1 ページ目は 299B で止め、2 ページ目を「あ」にする。
  std::string reply(299, 'a');
  reply += "あ";
  f.muse.set_reply(reply.c_str());
  f.tick(1000);
  ASSERT_EQ(watch::features::muse_pages(), 2u);
  EXPECT_EQ(f.page(), std::string(299, 'a'));
  watch::features::muse_next_page();
  EXPECT_EQ(f.page(), "あ");
  EXPECT_TRUE(valid_utf8(f.page()));
  EXPECT_EQ(f.all_pages(), reply);
}

TEST(Muse, ReplyIsCappedAtMaxBytes) {
  MuseFixture f;
  f.muse.set_state(MuseState::Ready);
  std::string reply(2000, 'x');
  f.muse.set_reply(reply.c_str());
  f.tick(1000);
  // 960B まで = 300+300+300+60 の 4 ページ。超過分は表示しない。
  EXPECT_EQ(watch::features::muse_pages(), 4u);
  EXPECT_EQ(f.all_pages().size(), kMuseReplyMax);
}

TEST(Muse, TruncatedMultibyteTailIsDropped) {
  MuseFixture f;
  f.muse.set_state(MuseState::Ready);
  // 959B 目が「あ」の先頭バイトになる返答。FakeMuse.last_reply は cap-1 で
  // バイト切りするので、feature が不完全な文字を落とすことを検査する。
  std::string reply(958, 'a');
  reply += "あ";
  reply += "い";
  f.muse.set_reply(reply.c_str());
  f.tick(1000);
  const std::string got = f.all_pages();
  EXPECT_EQ(got.size(), 958u);  // 不完全な「あ」の先頭 1B は捨てる
  EXPECT_TRUE(valid_utf8(got));
}

TEST(Muse, NewReplyResetsPageAndRevision) {
  MuseFixture f;
  f.muse.set_state(MuseState::Ready);
  f.muse.set_reply(std::string(700, 'a').c_str());
  f.tick(1000);
  EXPECT_EQ(watch::features::muse_reply_revision(), 1u);
  watch::features::muse_next_page();
  EXPECT_EQ(watch::features::muse_page(), 1u);

  f.muse.set_reply("新しい返答");
  f.tick(2000);
  EXPECT_EQ(watch::features::muse_reply_revision(), 2u);
  EXPECT_EQ(watch::features::muse_page(), 0u);
  EXPECT_EQ(watch::features::muse_pages(), 1u);
  EXPECT_EQ(f.page(), "新しい返答");
}

// ---- 自動ページ送り (15 秒) と画面表示中だけのポーリング ----

TEST(Muse, AutoAdvancesEveryFifteenSecondsWhileVisible) {
  MuseFixture f;
  f.muse.set_state(MuseState::Ready);
  f.muse.set_reply(std::string(700, 'a').c_str());
  f.tick(1000);  // 画面が見えた最初の tick (15 秒を数え直す)
  EXPECT_EQ(watch::features::muse_page(), 0u);
  EXPECT_EQ(kMuse.next_deadline_ms(), 2000);  // 1 秒周期で起こす

  f.tick(1000 + kMusePageMs - 1);
  EXPECT_EQ(watch::features::muse_page(), 0u);
  f.tick(1000 + kMusePageMs);
  EXPECT_EQ(watch::features::muse_page(), 1u);
  f.tick(1000 + 2 * kMusePageMs);
  EXPECT_EQ(watch::features::muse_page(), 2u);
  f.tick(1000 + 3 * kMusePageMs);
  EXPECT_EQ(watch::features::muse_page(), 0u);  // 最後まで行ったら先頭へ
}

TEST(Muse, DoesNotPollWhenScreenHidden) {
  MuseFixture f;
  f.muse.set_state(MuseState::Ready);
  f.muse.set_reply(std::string(700, 'a').c_str());
  f.tick(1000);
  EXPECT_EQ(kMuse.next_deadline_ms(), 2000);

  f.nav.pop();  // 別の画面へ
  f.tick(1000 + kMusePageMs);
  EXPECT_EQ(kMuse.next_deadline_ms(), 0);
  // 見えていない間はページが進まない。
  EXPECT_EQ(watch::features::muse_page(), 0u);

  // 戻ってきたらその時点から 15 秒を数え直す。
  f.nav.push(Route::App1);
  f.tick(20000);
  EXPECT_EQ(watch::features::muse_page(), 0u);
  f.tick(20000 + kMusePageMs);
  EXPECT_EQ(watch::features::muse_page(), 1u);
}

TEST(Muse, NoPageWhenPortUnavailable) {
  MuseFixture f;
  f.muse.available_ = false;
  f.muse.set_reply("返答");
  f.tick(1000);
  EXPECT_EQ(watch::features::muse_pages(), 0u);
  EXPECT_FALSE(watch::features::muse_page_text(nullptr, 0));
  EXPECT_EQ(kMuse.next_deadline_ms(), 0);
}

TEST(Muse, ResetClearsSnapshot) {
  MuseFixture f;
  f.muse.set_state(MuseState::Ready);
  f.muse.set_reply("返答");
  f.tick(1000);
  ASSERT_TRUE(watch::features::muse_available());
  watch::features::muse_reset_state();
  EXPECT_FALSE(watch::features::muse_available());
  EXPECT_EQ(watch::features::muse_state(), MuseState::Unavailable);
  EXPECT_EQ(watch::features::muse_pages(), 0u);
  EXPECT_EQ(watch::features::muse_reply_revision(), 0u);
}

// ---- 音声入力 (15 秒の音声ノート / ファイル保存なし) ----

TEST(Muse, VoiceToggleStartsOnlyWhenReady) {
  MuseFixture f;
  f.muse.set_state(MuseState::Ready);
  f.tick(1000);
  f.act(ActionType::MuseVoiceToggle);
  EXPECT_EQ(f.muse.voice_start_calls, 1);
  EXPECT_EQ(f.muse.voice_max_sec, watch::features::kMuseVoiceMaxSec);
  f.tick(2000);
  EXPECT_EQ(watch::features::muse_voice_state(), MuseVoiceState::Recording);
  // 録音中は ADPCM ポンプのために 20ms 周期で app タスクを起こす。
  EXPECT_EQ(kMuse.next_deadline_ms(), 2000 + watch::features::kMuseVoicePumpMs);
  EXPECT_GE(f.muse.voice_pump_calls, 1);
}

TEST(Muse, VoiceStartRequiresSessionReady) {
  MuseFixture f;
  f.muse.set_state(MuseState::Off);
  f.tick(1000);
  f.act(ActionType::MuseVoiceToggle);
  EXPECT_EQ(f.muse.voice_start_calls, 0);

  f.muse.set_state(MuseState::Busy);
  f.tick(2000);
  f.act(ActionType::MuseVoiceToggle);
  EXPECT_EQ(f.muse.voice_start_calls, 0);
}

TEST(Muse, VoiceToggleStopsAndSends) {
  MuseFixture f;
  f.muse.set_state(MuseState::Ready);
  f.tick(1000);
  f.act(ActionType::MuseVoiceToggle);
  f.tick(2000);
  ASSERT_EQ(watch::features::muse_voice_state(), MuseVoiceState::Recording);
  f.act(ActionType::MuseVoiceToggle);  // 停止 → 送信
  EXPECT_EQ(f.muse.voice_stop_calls, 1);
  f.tick(2100);
  EXPECT_EQ(watch::features::muse_voice_state(), MuseVoiceState::Sending);
}

TEST(Muse, VoiceAutoStopsAtFifteenSeconds) {
  MuseFixture f;
  f.muse.set_state(MuseState::Ready);
  f.tick(1000);
  f.act(ActionType::MuseVoiceToggle);
  f.tick(2000);
  ASSERT_EQ(watch::features::muse_voice_state(), MuseVoiceState::Recording);

  f.muse.voice_elapsed = watch::features::kMuseVoiceMaxSec - 1;
  f.tick(3000);
  EXPECT_EQ(f.muse.voice_stop_calls, 0);  // まだ止めない
  EXPECT_EQ(watch::features::muse_voice_state(), MuseVoiceState::Recording);

  f.muse.voice_elapsed = watch::features::kMuseVoiceMaxSec;
  f.tick(4000);
  EXPECT_EQ(f.muse.voice_stop_calls, 1);  // 15 秒で自動停止 → 送信
  f.tick(5000);
  EXPECT_EQ(watch::features::muse_voice_state(), MuseVoiceState::Sending);
}

TEST(Muse, VoicePumpKeepsRunningWhenScreenHidden) {
  MuseFixture f;
  f.muse.set_state(MuseState::Ready);
  f.tick(1000);
  f.act(ActionType::MuseVoiceToggle);
  f.tick(2000);
  ASSERT_EQ(watch::features::muse_voice_state(), MuseVoiceState::Recording);

  f.nav.pop();  // 録音中に別画面へ
  const int pumps = f.muse.voice_pump_calls;
  f.tick(3000);
  EXPECT_GT(f.muse.voice_pump_calls, pumps);  // ポンプは続く
  EXPECT_EQ(kMuse.next_deadline_ms(), 3000 + watch::features::kMuseVoicePumpMs);
}

TEST(Muse, VoiceErrorIsShown) {
  MuseFixture f;
  f.muse.set_state(MuseState::Ready);
  f.muse.set_voice(MuseVoiceState::Error, 0,
                   "PSRAM が不足しています（テーマを軽くしてください）");
  f.tick(1000);
  EXPECT_EQ(watch::features::muse_voice_state(), MuseVoiceState::Error);
  EXPECT_STREQ(watch::features::muse_voice_error(),
               "PSRAM が不足しています（テーマを軽くしてください）");
  // エラー後は再試行できる (Idle/Error で「話す」)。
  f.act(ActionType::MuseVoiceToggle);
  EXPECT_EQ(f.muse.voice_start_calls, 1);
}

TEST(Muse, ResetClearsVoiceState) {
  MuseFixture f;
  f.muse.set_state(MuseState::Ready);
  f.tick(1000);
  f.act(ActionType::MuseVoiceToggle);
  f.tick(2000);
  ASSERT_EQ(watch::features::muse_voice_state(), MuseVoiceState::Recording);
  watch::features::muse_reset_state();
  EXPECT_EQ(watch::features::muse_voice_state(), MuseVoiceState::Idle);
  EXPECT_EQ(watch::features::muse_voice_elapsed_s(), 0u);
  EXPECT_STREQ(watch::features::muse_voice_error(), "");
}
