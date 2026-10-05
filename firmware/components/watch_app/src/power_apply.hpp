// power_apply.hpp — PowerPolicy の状態をハードへ反映する内部層。
#pragma once

#include "watch/power.hpp"
#include "watch/settings.hpp"

namespace watch_app {

// 電源状態に応じて輝度・パネル・LVGLタイマーを切り替える。
// settings は復帰時のユーザー輝度に使う。
void power_apply(watch::PowerState st, const watch::Settings& settings);

}  // namespace watch_app
