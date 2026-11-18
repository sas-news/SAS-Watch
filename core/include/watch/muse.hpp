// muse.hpp — Muse (Phase 2 ⑤ UI 字幕) の core インターフェース。
//
// core は firmware (muse_link / Meta Muse SDK) を include できないので、
// セッションの実体は MusePort の実装として firmware 側から差し込む
// (FeatureContext::muse)。既定ビルド (CONFIG_WATCH_ENABLE_MUSE_LINK=n) では
// FeatureContext::muse は nullptr のまま = UI は「このファームでは Muse が
// 無効です」を出す。
//
// 秘密 (ホスト名/token) は core に渡さない。状態・日本語エラー・返答本文だけ。
#pragma once

#include <cstddef>
#include <cstdint>

namespace watch {

// UI 表示用の状態。firmware の enum はここに写像する (漏らさない)。
enum class MuseState : uint8_t {
  Unavailable = 0,  // ポートが無い/この個体では使えない
  Unconfigured,     // 資格情報が未設定 (ホスト/token が NVS に無い)
  Off,              // 停止中 (資格情報あり)
  Starting,         // 接続中 (Wi-Fi / VM セッション起動)
  Ready,            // 接続済み・待機
  Busy,             // 送信中/返答待ち
  Error,            // エラー (last_error() 参照)
};

// Muse セッションの実装 (firmware 側)。全メソッドは app タスクから呼ぶ。
// LVGL タスクからは呼ばない (実装はブロック/ロックする可能性がある)。
class MusePort {
 public:
  virtual ~MusePort() = default;

  // この個体で Muse が使えるか。false なら UI は無効表示にする。
  virtual bool available() const = 0;

  virtual MuseState state() = 0;

  // 日本語の短いエラー (Error 以外では空のことが多い)。秘密は含めない。
  virtual const char* last_error() = 0;

  // 最新の返答が更新されるたびに増える世代番号 (0 = まだ無い)。
  // UI は「世代が変わったときだけ」last_reply() で取り直す。
  virtual uint32_t reply_generation() = 0;

  // 最新の返答を out にコピーし、コピーしたバイト数を返す (NUL 終端する)。
  // cap を超える場合は切り詰める。返答が無ければ 0。
  virtual size_t last_reply(char* out, size_t cap) = 0;

  // セッション開始。即座に返る (進捗は state() が Starting→Ready/Error)。
  // 既に開始中/接続済みなら false。
  virtual bool start() = 0;

  // セッション停止 (Wi-Fi の release などは実装側の責務)。
  virtual void stop() = 0;

  // テキスト送信。この UI ではまだ使わない (音声/テキスト入力は次の段階)。
  virtual bool send_text(const char* text) = 0;
};

}  // namespace watch
