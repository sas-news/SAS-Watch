// muse_port.cpp — core MusePort ← muse_link アダプタ (Kconfig on のみ)。
//
// 役割:
//   - muse_link のセッション状態を core の MuseState に写像する
//   - 最新の返答を core に渡す (世代番号は muse_link の返答コールバックで進める)
//   - start/stop/send_text を muse_link に流す
// 秘密 (host/vm/token) は core に渡さない。日本語エラーだけを渡す。
#include "muse_port.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "audio/audio.hpp"
#include "muse_link/muse_link.h"
#include "watch/adpcm.hpp"

namespace watch_app {
namespace {

// Wi-Fi 接続待ちの最大時間。muse_link_start 自体は専用タスクを起動して
// 即座に返る (docs/muse-bmin.md §12.1)。
// TODO(hw): 実機で Wi-Fi 接続待ち (起動〜ready) の実測時間を確認して調整する。
constexpr uint32_t kWifiTimeoutMs = 20000;

// 音声入力 (T57)。15 秒の音声ノートを PSRAM 上で PCM にして送る。
// 音声はファイルに保存しない (AudioPort の stream 経路のみ)。
constexpr uint32_t kMuseVoiceMaxSec = 15;
constexpr uint32_t kMuseVoiceFileId = 0x80000001u;  // 録音ジョブ用の仮 id
// ADPCM リングの 1 回の読み出し量 (256 samples)。小さいほどスタックが軽い。
constexpr size_t kAdpcmChunkBytes = 128;  // = 256 PCM samples
constexpr size_t kPcmChunkSamples = kAdpcmChunkBytes * 2;
// 1 回のポンプで読むチャンク数の上限 (20ms 周期で 8KB/s を余裕で賄う)。
constexpr size_t kPumpChunksMax = 16;

class EspMusePort : public watch::MusePort {
 public:
  EspMusePort() {
    // 返答が更新されたら世代番号を進める (SDK の受信タスクから呼ばれる)。
    muse_link_set_reply_callback(&EspMusePort::on_reply, this);
  }

  bool available() const override { return true; }

  watch::MuseState state() override {
    switch (muse_link_session_state()) {
      case MUSE_LINK_SESSION_STARTING:
        return watch::MuseState::Starting;
      case MUSE_LINK_SESSION_READY:
        return watch::MuseState::Ready;
      case MUSE_LINK_SESSION_BUSY:
        return watch::MuseState::Busy;
      case MUSE_LINK_SESSION_ERROR:
        return watch::MuseState::Error;
      case MUSE_LINK_SESSION_OFF:
      default:
        // 停止中でも資格情報が無ければ UI は「未設定」を出す。
        muse_link_status_t st = {};
        muse_link_status(&st);
        return st.configured ? watch::MuseState::Off
                             : watch::MuseState::Unconfigured;
    }
  }

  const char* last_error() override { return muse_link_last_error(); }

  uint32_t reply_generation() override { return generation_.load(); }

  size_t last_reply(char* out, size_t cap) override {
    return muse_link_last_reply(out, cap);  // バイト切りは core が UTF-8 で直す
  }

  bool start() override { return muse_link_start(kWifiTimeoutMs); }

  void stop() override { muse_link_stop(); }

  bool send_text(const char* text) override {
    return text && *text ? muse_link_send_text(text) : false;
  }

  // ---- 音声入力 (15 秒の音声ノート / ファイル保存なし) ----
  watch::MuseVoiceState voice_state() override {
    if (recording_) return watch::MuseVoiceState::Recording;
    if (error_[0] != '\0') return watch::MuseVoiceState::Error;
    switch (muse_link_voice_state()) {
      case MUSE_LINK_VOICE_RECORDING:
        return watch::MuseVoiceState::Recording;
      case MUSE_LINK_VOICE_SENDING:
        return watch::MuseVoiceState::Sending;
      case MUSE_LINK_VOICE_ERROR:
        return watch::MuseVoiceState::Error;
      case MUSE_LINK_VOICE_IDLE:
      default:
        return watch::MuseVoiceState::Idle;
    }
  }

  bool voice_start(uint32_t max_sec) override {
    if (recording_) return false;
    error_[0] = '\0';
    watch::AudioPort* a = audio::port();
    if (!a) {
      set_voice_error("この個体ではマイクを使えません");
      return false;
    }
    if (!a->can_record_stream()) {
      set_voice_error("音声ストリームを使えません");
      return false;
    }
    // セッションが Ready のときだけ話せる (録音開始は app タスクから)。
    if (muse_link_session_state() != MUSE_LINK_SESSION_READY) {
      set_voice_error("Muse に接続してから話してください");
      return false;
    }
    if (muse_link_voice_state() == MUSE_LINK_VOICE_SENDING) {
      set_voice_error("前の音声を送信中です");
      return false;
    }
    if (max_sec < 1) max_sec = 1;
    if (max_sec > kMuseVoiceMaxSec) max_sec = kMuseVoiceMaxSec;
    max_sec_ = max_sec;
    // T38 の「録音ストリーム (ADPCM, ファイル無し)」経路だけを使う。
    if (!a->record_stream_enable(true)) {
      set_voice_error("音声入力を開始できません");
      return false;
    }
    if (!a->record_begin(kMuseVoiceFileId, max_sec_)) {
      a->record_stream_enable(false);
      set_voice_error("音声入力を開始できません");
      return false;
    }
    // PSRAM の PCM バッファ確保と状態機械は muse_link 側 (T57)。
    if (!muse_link_voice_start(max_sec_)) {
      uint32_t sec = 0, size = 0;
      a->record_end(false, &sec, &size);
      a->record_stream_enable(false);
      const char* e = muse_link_voice_error();
      set_voice_error((e && *e) ? e : "音声バッファを確保できません");
      return false;
    }
    dec_.reset();
    expected_offset_ = 0;
    recording_ = true;
    return true;
  }

  void voice_stop() override {
    if (!recording_) return;
    drain_ring(true);
    audio_stop();
    muse_link_voice_stop();
    if (muse_link_voice_state() == MUSE_LINK_VOICE_ERROR) {
      const char* e = muse_link_voice_error();
      set_voice_error((e && *e) ? e : "音声を送信できませんでした");
    }
  }

  uint32_t voice_elapsed_s() override {
    if (!recording_) {
      return 0;
    }
    return muse_link_voice_elapsed_s();
  }

  const char* voice_error() override {
    if (error_[0] != '\0') return error_;
    return muse_link_voice_error();
  }

  // 録音中だけ呼ばれる (core の feature が ~20ms 周期で回す)。
  void voice_pump() override {
    if (!recording_) return;
    watch::AudioPort* a = audio::port();
    if (!a) {
      abort_voice("この個体ではマイクを使えません");
      return;
    }
    // 録音デバイス側が先に止まった (失敗/上限) 場合。
    if (!a->recording()) {
      abort_voice("マイクが停止しました");
      return;
    }
    if (!drain_ring(false)) {
      // PCM 容量超過など。理由は muse_link / 自前のエラーに入っている。
      const char* e = muse_link_voice_error();
      abort_voice((e && *e) ? e : "音声を蓄積できませんでした");
      return;
    }
  }

 private:
  static void on_reply(size_t /*total_len*/, void* ctx) {
    static_cast<EspMusePort*>(ctx)->generation_.fetch_add(1);
  }

  void set_voice_error(const char* msg) {
    std::snprintf(error_, sizeof(error_), "%s", msg ? msg : "");
  }

  // ADPCM リングを読み、PCM にデコードして muse_link に積む。
  // full=true ならリングが空になるまで (停止時の吐き切り)。
  // 戻り値 false = 致命的 (容量超過など。エラー文言は設定済み)。
  bool drain_ring(bool full) {
    watch::AudioPort* a = audio::port();
    if (!a) return false;
    const size_t max_chunks = full ? 512 : kPumpChunksMax;
    for (size_t i = 0; i < max_chunks; ++i) {
      const uint32_t off = a->record_stream_offset();
      if (off != expected_offset_) {
        // リングが溢れて古い音声が捨てられた (T38)。デコーダ状態を切る。
        dec_.reset();
        expected_offset_ = off;
      }
      uint8_t adpcm[kAdpcmChunkBytes];
      size_t n = sizeof(adpcm);
      if (!a->record_stream_read(adpcm, &n)) {
        return false;  // ストリームが無効化された
      }
      if (n == 0) return true;  // 吐き切った
      int16_t pcm[kPcmChunkSamples];
      const size_t got = dec_.decode(adpcm, n, pcm, kPcmChunkSamples);
      if (got > 0 && !muse_link_voice_append(pcm, got)) {
        const char* e = muse_link_voice_error();
        if (!e || !*e) set_voice_error("音声が長すぎます (15 秒まで)");
        return false;
      }
      expected_offset_ = off + static_cast<uint32_t>(n);
    }
    return true;
  }

  void audio_stop() {
    watch::AudioPort* a = audio::port();
    if (!a) return;
    if (a->recording()) {
      uint32_t sec = 0, size = 0;
      a->record_end(false, &sec, &size);
    }
    a->record_stream_enable(false);  // リングを解放 (音声は保存しない)
    recording_ = false;
  }

  // エラー時: 録音を畳んで破棄し、日本語エラーを残す。
  void abort_voice(const char* msg) {
    audio_stop();
    muse_link_voice_cancel();
    set_voice_error(msg);
  }

  std::atomic<uint32_t> generation_{0};
  watch::audio::ImaAdpcmDecoder dec_;
  bool recording_ = false;
  uint32_t expected_offset_ = 0;  // 次に読むべき ring の累積 offset
  uint32_t max_sec_ = kMuseVoiceMaxSec;
  char error_[96] = {};
};

EspMusePort s_port;

}  // namespace

watch::MusePort* muse_port() { return &s_port; }

}  // namespace watch_app
