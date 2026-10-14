// template_app — 雛形アプリの Feature 宣言。
// Feature は LVGL を知らない。状態はこの .cpp 内の static か
// ctx.storage (KeyValueStore、キーは15文字まで) に持つ。
#pragma once
#include "watch/feature.hpp"

namespace watch {
namespace features {

// applist の行 (生成コードが参照するので extern)
extern const FeatureDescriptor kTemplateApp;

// 画面が読む状態 (この例ではカウント)
int32_t template_app_count();

}  // namespace features
}  // namespace watch
