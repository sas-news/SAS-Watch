// muse_port.cpp — core MusePort ← muse_link アダプタ (Kconfig on のみ)。
//
// 役割:
//   - muse_link のセッション状態を core の MuseState に写像する
//   - 最新の返答を core に渡す (世代番号は muse_link の返答コールバックで進める)
//   - start/stop/send_text を muse_link に流す
// 秘密 (host/vm/token) は core に渡さない。日本語エラーだけを渡す。
#include "muse_port.hpp"

#include <atomic>

#include "muse_link/muse_link.h"

namespace watch_app {
namespace {

// Wi-Fi 接続待ちの最大時間。muse_link_start 自体は専用タスクを起動して
// 即座に返る (docs/muse-bmin.md §12.1)。
// TODO(hw): 実機で Wi-Fi 接続待ち (起動〜ready) の実測時間を確認して調整する。
constexpr uint32_t kWifiTimeoutMs = 20000;

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

 private:
  static void on_reply(size_t /*total_len*/, void* ctx) {
    static_cast<EspMusePort*>(ctx)->generation_.fetch_add(1);
  }

  std::atomic<uint32_t> generation_{0};
};

EspMusePort s_port;

}  // namespace

watch::MusePort* muse_port() { return &s_port; }

}  // namespace watch_app
