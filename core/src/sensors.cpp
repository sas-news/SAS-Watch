// sensors.cpp — IMU 判定ロジック実装。浮動小数点 + sqrtf/acosf のみ。
// 角度は内積 → acosf で求める (外れ値は clamp)。
#include "watch/sensors.hpp"

#include <cmath>

namespace watch {
namespace sensors {

namespace {

float clamp1(float v) { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); }

// rad → deg。
float deg_of(float dot) { return std::acos(clamp1(dot)) * 57.2957795f; }

}  // namespace

// ---- RaiseDetector ---------------------------------------------------------

void RaiseDetector::reset() {
  ref_valid_ = false;
  hold_since_ms_ = 0;
  raised_ = false;
  fired_ms_ = 0;
}

bool RaiseDetector::feed(int16_t x_mg, int16_t y_mg, int16_t z_mg,
                         int64_t now_ms) {
  const float mag =
      std::sqrt(static_cast<float>(x_mg) * x_mg +
                static_cast<float>(y_mg) * y_mg +
                static_cast<float>(z_mg) * z_mg) /
      1000.0f;  // g
  // 向きが信頼できる範囲外 (加速度が大きすぎ/小さすぎ) は判定を保留。
  if (mag < cfg_.min_g || mag > cfg_.max_g) {
    hold_since_ms_ = 0;
    return false;
  }
  const float cx = x_mg / 1000.0f / mag;
  const float cy = y_mg / 1000.0f / mag;
  const float cz = z_mg / 1000.0f / mag;

  if (!ref_valid_) {
    rx_ = cx;
    ry_ = cy;
    rz_ = cz;
    ref_valid_ = true;
    return false;
  }

  const float ang = deg_of(cx * rx_ + cy * ry_ + cz * rz_);

  if (raised_) {
    // 検出直後: クールダウン経過 & 安静角度に戻るまで再検出しない。
    if (now_ms - fired_ms_ < cfg_.cooldown_ms) return false;
    if (ang < cfg_.return_deg) {
      raised_ = false;
      hold_since_ms_ = 0;
    }
    return false;
  }

  if (ang >= cfg_.raise_deg) {
    if (hold_since_ms_ == 0) {
      hold_since_ms_ = now_ms;
      return false;
    }
    if (now_ms - hold_since_ms_ >= cfg_.hold_ms) {
      raised_ = true;
      fired_ms_ = now_ms;
      hold_since_ms_ = 0;
      return true;
    }
    return false;
  }

  hold_since_ms_ = 0;
  if (ang < cfg_.still_deg) {
    // 安静時: 基準ベクトルをゆっくり追従 (装着角度の日変化に追いつく)。
    rx_ += (cx - rx_) * cfg_.base_lerp;
    ry_ += (cy - ry_) * cfg_.base_lerp;
    rz_ += (cz - rz_) * cfg_.base_lerp;
    const float n = std::sqrt(rx_ * rx_ + ry_ * ry_ + rz_ * rz_);
    if (n > 0.01f) {
      rx_ /= n;
      ry_ /= n;
      rz_ /= n;
    }
  }
  return false;
}

// ---- StepDetector ----------------------------------------------------------

void StepDetector::reset() {
  base_ = 0;
  init_ = false;
  armed_ = true;
  last_step_ms_ = 0;
}

bool StepDetector::feed(int16_t x_mg, int16_t y_mg, int16_t z_mg,
                        int64_t now_ms) {
  const float mag =
      std::sqrt(static_cast<float>(x_mg) * x_mg +
                static_cast<float>(y_mg) * y_mg +
                static_cast<float>(z_mg) * z_mg);  // mg
  if (!init_) {
    base_ = mag;
    init_ = true;
    return false;
  }
  base_ += (mag - base_) * cfg_.base_lerp;
  const float dyn = mag - base_;

  if (armed_ && dyn > cfg_.peak_mg &&
      now_ms - last_step_ms_ >= cfg_.refractory_ms) {
    last_step_ms_ = now_ms;
    armed_ = false;
    return true;
  }
  if (!armed_ && dyn < cfg_.valley_mg) {
    armed_ = true;
  }
  return false;
}

}  // namespace sensors
}  // namespace watch
