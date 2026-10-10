// Media Feature。曲情報の保持と、操作ボタン → MediaCmdRequested の変換。
#include "watch/features/media.hpp"

#include <cstring>

namespace watch {
namespace features {

namespace {

MediaState g_state;

void copy_str(char* dst, size_t cap, const char* s) {
  if (!s) {
    dst[0] = '\0';
    return;
  }
  size_t n = std::strlen(s);
  if (n >= cap) n = cap - 1;
  std::memcpy(dst, s, n);
  dst[n] = '\0';
}

bool handle(const Action& a, FeatureContext& ctx) {
  if (a.type != ActionType::MediaCommand) return false;
  // コマンドはそのまま Event 化 (ble_glue が EVT "media.cmd" に変換)。
  ctx.bus.publish({EventType::MediaCmdRequested, a.arg0});
  return true;
}

}  // namespace

const MediaState& media_state() { return g_state; }

void media_set(const char* title, const char* artist, bool playing,
               FeatureContext& ctx) {
  copy_str(g_state.title, sizeof(g_state.title), title);
  copy_str(g_state.artist, sizeof(g_state.artist), artist);
  g_state.playing = playing;
  g_state.valid = true;
  ctx.bus.publish({EventType::MediaStateChanged, 0});
}

void media_reset_state() { g_state = MediaState{}; }

const FeatureDescriptor kMedia = {
    /*id*/ "media",
    /*capabilities*/ HasScreen | HasQuickTile,
    /*route*/ Route::Media,
    /*init*/ nullptr,
    /*handle*/ handle,
    /*tick*/ nullptr,
    /*next_deadline_ms*/ nullptr,
    /*save*/ nullptr,
    /*restore*/ nullptr,
};

}  // namespace features
}  // namespace watch
