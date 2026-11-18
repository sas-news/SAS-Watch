// muse.cpp — Muse アプリ (Phase 2 ⑤ UI 字幕) の Feature。
//
// セッションは firmware の MusePort (muse_link) が持ち、ここは
//   1) Port の状態・エラー・返答を取り込む (状態スナップショット)
//   2) 返答を UTF-8 境界でページ分割し、15 秒ごとに次ページへ送る
//   3) PrimaryAction (接続/切断) と LongPress (切断) を Port に流す
// だけを行う。永続化はしない (返答はサーバが正。再起動後は取り直す)。
//
// 画面が見えている間だけ 1 秒周期で app タスクを起こす (next_deadline)。
// 1 秒ごとの ClockTick で画面が再描画され、ページ送りもその中で進む。
#include "watch/features/muse.hpp"

#include <cstring>

#include "watch/power.hpp"

namespace watch {
namespace features {

namespace {

// 状態スナップショット (app タスクが書き、画面が読む。walk と同じ流儀)。
struct Snapshot {
  bool available = false;
  MuseState state = MuseState::Unavailable;
  char error[kMuseErrorMax] = {};
  char reply[kMuseReplyMax + 1] = {};
  uint32_t revision = 0;   // 返答が更新されるたびに増える
  uint16_t page = 0;       // 現在ページ (0 始まり)
  uint16_t pages = 0;      // 総ページ数
  // 音声入力 (15 秒の音声ノート。音声は保存しない)
  MuseVoiceState voice = MuseVoiceState::Idle;
  uint32_t voice_elapsed_s = 0;
  char voice_error[kMuseErrorMax] = {};
};
Snapshot s_snap;

// ページ開始位置 (UTF-8 境界)。s_page_start[0]=0, [pages]=本文長。
uint16_t s_page_start[kMuseMaxPages + 1] = {};
int64_t s_now_ms = 0;         // 直近 tick の monotonic
int64_t s_wake_ms = 0;        // 次の起床期限
int64_t s_voice_wake_ms = 0;  // 音声入力ポンプの次回起床
int64_t s_page_ms = 0;        // 現在ページを表示し始めた時刻
uint32_t s_gen = 0;           // 取り込み済みの port 世代
bool s_polling = false;       // 画面が見えているか
bool s_was_visible = false;

// UTF-8 の先頭バイトから、その文字のバイト数 (不正なら 1)。
size_t utf8_char_len(uint8_t lead) {
  if (lead < 0x80) return 1;
  if ((lead & 0xE0) == 0xC0) return 2;
  if ((lead & 0xF0) == 0xE0) return 3;
  if ((lead & 0xF8) == 0xF0) return 4;
  return 1;
}

// dst に src を最大 cap-1 バイトコピーし、UTF-8 の途中で切らない。
void copy_utf8(char* dst, size_t cap, const char* src) {
  size_t n = src ? std::strlen(src) : 0;
  if (n > cap - 1) n = cap - 1;
  size_t head = n;
  while (head > 0 && (static_cast<uint8_t>(src[head - 1]) & 0xC0) == 0x80) {
    --head;
  }
  if (head > 0) {
    const uint8_t lead = static_cast<uint8_t>(src[head - 1]);
    const size_t need = utf8_char_len(lead);
    if (n - (head - 1) < need) n = head - 1;
  }
  if (n > 0) std::memcpy(dst, src, n);
  dst[n] = '\0';
}

// from 以降で 1 ページ分の終端 (UTF-8 境界) を返す。
size_t page_end(size_t from, size_t len) {
  size_t pos = from;
  while (pos < len) {
    const size_t clen = utf8_char_len(static_cast<uint8_t>(s_snap.reply[pos]));
    if (pos + clen > from + kMusePageBytes && pos > from) break;
    pos += clen;
  }
  return pos;
}

// 返答をページに分割する (本文が変わったときだけ呼ぶ)。
void build_pages(size_t len) {
  size_t pos = 0;
  uint16_t n = 0;
  s_page_start[0] = 0;
  while (pos < len && n < kMuseMaxPages) {
    pos = page_end(pos, len);
    s_page_start[++n] = static_cast<uint16_t>(pos);
  }
  s_snap.pages = n;
  s_snap.page = 0;
}

// Port の状態を取り込む。返答は世代が変わったときだけコピーする。
void update_from_port(FeatureContext& c, int64_t now_ms) {
  if (!c.muse || !c.muse->available()) {
    s_snap.available = false;
    s_snap.state = MuseState::Unavailable;
    s_snap.error[0] = '\0';
    s_snap.reply[0] = '\0';
    s_snap.pages = 0;
    s_snap.page = 0;
    s_snap.voice = MuseVoiceState::Idle;
    s_snap.voice_elapsed_s = 0;
    s_snap.voice_error[0] = '\0';
    return;
  }
  s_snap.available = true;
  s_snap.state = c.muse->state();
  copy_utf8(s_snap.error, sizeof(s_snap.error), c.muse->last_error());

  // 音声入力の状態 (録音中/送信中/エラー)。
  s_snap.voice = c.muse->voice_state();
  s_snap.voice_elapsed_s = c.muse->voice_elapsed_s();
  copy_utf8(s_snap.voice_error, sizeof(s_snap.voice_error),
            c.muse->voice_error());

  const uint32_t gen = c.muse->reply_generation();
  if (gen != s_gen) {
    s_gen = gen;
    // cap は NUL 込み。960B まで取り込むためバッファ全体を渡す。
    size_t n = c.muse->last_reply(s_snap.reply, sizeof(s_snap.reply));
    if (n > kMuseReplyMax) n = kMuseReplyMax;
    s_snap.reply[n] = '\0';
    // 実装によってはバイト境界で切られるので、末尾の不完全な文字を落とす。
    size_t head = n;
    while (head > 0 &&
           (static_cast<uint8_t>(s_snap.reply[head - 1]) & 0xC0) == 0x80) {
      --head;
    }
    if (head > 0) {
      const size_t need =
          utf8_char_len(static_cast<uint8_t>(s_snap.reply[head - 1]));
      if (n - (head - 1) < need) n = head - 1;
    }
    s_snap.reply[n] = '\0';
    ++s_snap.revision;
    build_pages(n);
    s_page_ms = now_ms;
  }
}

void next_page(int64_t now_ms) {
  if (s_snap.pages <= 1) return;
  s_snap.page = static_cast<uint16_t>((s_snap.page + 1) % s_snap.pages);
  s_page_ms = now_ms;
}

void tick(int64_t now_ms, FeatureContext& c) {
  s_now_ms = now_ms;
  update_from_port(c, now_ms);

  // 音声入力は画面から離れても止めない (15 秒で自動停止)。録音中は
  // 20ms 周期で ADPCM リングを PCM にデコードする (app タスクのポンプ)。
  s_voice_wake_ms = 0;
  if (c.muse && s_snap.voice == MuseVoiceState::Recording) {
    c.muse->voice_pump();
    if (c.muse->voice_elapsed_s() >= kMuseVoiceMaxSec) {
      c.muse->voice_stop();  // 15 秒で自動停止 → 送信
    }
    s_voice_wake_ms = now_ms + kMuseVoicePumpMs;
  }

  const bool visible =
      c.nav && c.nav->current() == kMuse.route && !c.nav->has_modal() &&
      c.power && c.power->state() == PowerState::Active;
  // Port が無い既定ビルドでは状態が変わらないのでポーリングしない (電池優先)。
  s_polling = visible && c.muse != nullptr && c.muse->available();
  if (!visible) {
    s_was_visible = false;
    return;
  }
  if (!s_was_visible) {
    // 画面を開いた時点から 15 秒を数え直す (開いた瞬間に飛ばない)。
    s_was_visible = true;
    s_page_ms = now_ms;
  }
  if (s_polling) s_wake_ms = now_ms + 1000;
  if (s_snap.pages > 1 && now_ms - s_page_ms >= kMusePageMs) {
    next_page(now_ms);
  }
}

int64_t next_deadline() {
  int64_t d = s_polling ? s_wake_ms : 0;
  if (s_voice_wake_ms != 0 && (d == 0 || s_voice_wake_ms < d)) {
    d = s_voice_wake_ms;
  }
  return d;
}

bool handle(const Action& a, FeatureContext& c) {
  if (a.type == ActionType::PrimaryAction) {
    if (!c.muse || !c.muse->available()) return true;  // 「無効」表示のみ
    const MuseState st = c.muse->state();
    if (st == MuseState::Off || st == MuseState::Error) {
      muse_request_start(c);
    } else if (st == MuseState::Starting || st == MuseState::Ready ||
               st == MuseState::Busy) {
      muse_request_stop(c);
    }
    return true;
  }
  if (a.type == ActionType::MuseVoiceToggle) {
    muse_voice_toggle(c);
    return true;
  }
  if (a.type == ActionType::LongPress) {
    if (c.muse) muse_request_stop(c);
    return true;
  }
  return false;
}

}  // namespace

// ---- 画面用アクセサ ----
bool muse_available() { return s_snap.available; }
MuseState muse_state() { return s_snap.state; }
const char* muse_error() { return s_snap.error; }
uint16_t muse_page() { return s_snap.page; }
uint16_t muse_pages() { return s_snap.pages; }
uint32_t muse_reply_revision() { return s_snap.revision; }

bool muse_page_text(char* out, size_t cap) {
  if (!out || cap == 0) return false;
  out[0] = '\0';
  if (s_snap.pages == 0 || s_snap.page >= s_snap.pages) return false;
  const size_t start = s_page_start[s_snap.page];
  const size_t end = s_page_start[s_snap.page + 1];
  size_t n = end - start;
  if (n > cap - 1) n = cap - 1;
  std::memcpy(out, s_snap.reply + start, n);
  out[n] = '\0';
  return true;
}

void muse_next_page() { next_page(s_now_ms); }

MuseVoiceState muse_voice_state() { return s_snap.voice; }
uint32_t muse_voice_elapsed_s() { return s_snap.voice_elapsed_s; }
const char* muse_voice_error() { return s_snap.voice_error; }

void muse_voice_toggle(FeatureContext& c) {
  if (!c.muse || !c.muse->available()) return;
  const MuseVoiceState vs = c.muse->voice_state();
  if (vs == MuseVoiceState::Recording) {
    c.muse->voice_stop();  // 停止 → Muse へ送信
  } else if (vs == MuseVoiceState::Idle || vs == MuseVoiceState::Error) {
    // セッションが Ready のときだけ話せる (UI 側でもボタンを出し分ける)。
    if (c.muse->state() != MuseState::Ready) return;
    c.muse->voice_start(kMuseVoiceMaxSec);
  }
  update_from_port(c, s_now_ms);
}

void muse_request_start(FeatureContext& c) {
  if (!c.muse || !c.muse->available()) return;
  c.muse->start();
  update_from_port(c, s_now_ms);
}

void muse_request_stop(FeatureContext& c) {
  if (!c.muse) return;
  c.muse->stop();
  update_from_port(c, s_now_ms);
}

bool muse_request_send_text(FeatureContext& c, const char* text) {
  if (!c.muse || !text || !*text) return false;
  return c.muse->send_text(text);
}

void muse_reset_state() {
  s_snap = Snapshot{};
  std::memset(s_page_start, 0, sizeof(s_page_start));
  s_now_ms = 0;
  s_wake_ms = 0;
  s_page_ms = 0;
  s_gen = 0;
  s_polling = false;
  s_was_visible = false;
  s_voice_wake_ms = 0;
}

const FeatureDescriptor kMuse = {
    /*id*/ "muse",
    /*capabilities*/ HasScreen,
    /*route*/ Route::App1,  // app.cmake で明示 (walk が App0 を維持)
    /*init*/ nullptr,
    /*handle*/ handle,
    /*tick*/ tick,
    /*next_deadline_ms*/ next_deadline,
    /*save*/ nullptr,     // 返答はサーバが正。永続化しない
    /*restore*/ nullptr,
};

}  // namespace features
}  // namespace watch
