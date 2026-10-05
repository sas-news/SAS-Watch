// features/media.hpp — 音楽操作。media.state で届く曲情報を保持し、
// MediaCommand アクションを MediaCmdRequested Event に変換する
// (EVT "media.cmd" としてスマホへ返すのは ble_glue)。
#pragma once

#include "watch/feature.hpp"

namespace watch {
namespace features {

constexpr size_t kMediaTitleMax = 96;
constexpr size_t kMediaArtistMax = 96;

struct MediaState {
  char title[kMediaTitleMax] = {};
  char artist[kMediaArtistMax] = {};
  bool playing = false;
  bool valid = false;  // media.state を一度でも受け取ったか
};

extern const FeatureDescriptor kMedia;

const MediaState& media_state();
// media.state 受信時に呼ぶ (protocol 経由)。
void media_set(const char* title, const char* artist, bool playing,
               FeatureContext& ctx);

void media_reset_state();

}  // namespace features
}  // namespace watch
