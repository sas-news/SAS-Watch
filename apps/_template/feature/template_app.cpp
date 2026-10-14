// template_app — 最小 Feature。+1 でカウントを増やすだけ。
#include "template_app.hpp"

namespace watch {
namespace features {
namespace {
int32_t g_count = 0;

bool handle(const Action& a, FeatureContext& ctx) {
  // 画面の主アクション (BOOT 短押し / 画面のボタン) で +1。
  if (a.type == ActionType::PrimaryAction ||
      a.type == ActionType::CounterAdd) {
    ++g_count;
    ctx.bus.publish({EventType::SettingsChanged, 0});  // 画面へ通知 (汎用イベントで代用)
    return true;
  }
  return false;
}
}  // namespace

int32_t template_app_count() { return g_count; }

const FeatureDescriptor kTemplateApp = {
    /*id*/ "template",
    /*capabilities*/ HasScreen,
    /*route*/ Route::App0,    // 自動採番された Route (app.cmake で省略した)
    /*init*/ nullptr,
    /*handle*/ handle,
    /*tick*/ nullptr,
    /*next_deadline_ms*/ nullptr,
    /*save*/ nullptr,
    /*restore*/ nullptr,
    /*keep_screen_on*/ nullptr,
};

}  // namespace features
}  // namespace watch
