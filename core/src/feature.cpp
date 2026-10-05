#include "watch/feature.hpp"

#include <cstring>

namespace watch {

const FeatureDescriptor* FeatureRegistry::find(const char* id) const {
  if (!id) return nullptr;
  for (size_t i = 0; i < count_; ++i) {
    if (descs_[i] && descs_[i]->id && std::strcmp(descs_[i]->id, id) == 0) {
      return descs_[i];
    }
  }
  return nullptr;
}

const FeatureDescriptor* FeatureRegistry::find_by_route(Route r) const {
  for (size_t i = 0; i < count_; ++i) {
    if (descs_[i] && descs_[i]->route == r) return descs_[i];
  }
  return nullptr;
}

void FeatureRegistry::init_all(FeatureContext& ctx) const {
  for (size_t i = 0; i < count_; ++i) {
    if (descs_[i] && descs_[i]->init) descs_[i]->init(ctx);
  }
}

bool FeatureRegistry::handle(const Action& a, FeatureContext& ctx) const {
  for (size_t i = 0; i < count_; ++i) {
    if (descs_[i] && descs_[i]->handle && descs_[i]->handle(a, ctx)) {
      return true;
    }
  }
  return false;
}

void FeatureRegistry::tick_all(int64_t now_ms, FeatureContext& ctx) const {
  for (size_t i = 0; i < count_; ++i) {
    if (descs_[i] && descs_[i]->tick) descs_[i]->tick(now_ms, ctx);
  }
}

int64_t FeatureRegistry::next_deadline_ms() const {
  int64_t best = 0;
  for (size_t i = 0; i < count_; ++i) {
    if (descs_[i] && descs_[i]->next_deadline_ms) {
      const int64_t d = descs_[i]->next_deadline_ms();
      if (d > 0 && (best == 0 || d < best)) best = d;
    }
  }
  return best;
}

void FeatureRegistry::save_all(FeatureContext& ctx) const {
  for (size_t i = 0; i < count_; ++i) {
    if (descs_[i] && descs_[i]->save) descs_[i]->save(ctx);
  }
}

void FeatureRegistry::restore_all(FeatureContext& ctx) const {
  for (size_t i = 0; i < count_; ++i) {
    if (descs_[i] && descs_[i]->restore) descs_[i]->restore(ctx);
  }
}

}  // namespace watch
