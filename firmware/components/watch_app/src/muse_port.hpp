// muse_port.hpp — core の MusePort を muse_link で実装するアダプタ。
//
// CONFIG_WATCH_ENABLE_MUSE_LINK=y のときだけビルドされ、
// watch_app が FeatureContext::muse に差す (off は nullptr = UI は「無効」表示)。
// 実セッション (Wi-Fi / Noise / TLS) は muse_link 側が持つ。
#pragma once

#include "watch/muse.hpp"

namespace watch_app {

// シングルトン。初回呼び出しで返答コールバックを muse_link に登録する。
// app タスクから呼ぶこと (LVGL タスクからは呼ばない)。
watch::MusePort* muse_port();

}  // namespace watch_app
