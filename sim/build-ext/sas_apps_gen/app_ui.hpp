// GENERATED — apps/*/app.cmake から自動生成。編集しない。
#pragma once
#include <cstddef>
#include <cstdint>
#include "watch/navigation.hpp"
namespace ui {
struct ScreenOps;
// app.cmake 由来の画面 → ScreenOps。無ければ nullptr。
const ScreenOps* app_screen_ops(watch::Route r);
struct SasAppRow {
  const char* id;
  const char* icon;
  uint32_t icon_bg;
  const char* name;
  const char* (*sub)();
  watch::Route route;
};
extern const SasAppRow kSasAppRows[];
constexpr size_t kSasAppRowCount = 10;
}  // namespace ui
