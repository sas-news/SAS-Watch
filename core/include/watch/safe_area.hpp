#pragma once
// safe_area.hpp — 丸角パネル (410x502, 角 R≈100) の可視領域判定。
// 角は半径 R の円弧で、円の中心は各辺から R 内側。
// 「角点が円弧より margin 以上内側」= 角の円中心からの距離 <= R - margin。

namespace watch {

namespace safe_area {
constexpr int kWidth = 410;
constexpr int kHeight = 502;
constexpr int kRadius = 100;
}  // namespace safe_area

// 点 (x,y) が角の円弧より margin 以上内側か。角の帯 (x<R かつ y<R の角の
// 正方形領域) 外の点は円弧に関係なく安全 (直線辺) とみなす。
inline bool point_in_safe_area(int x, int y, int margin = 6) {
  constexpr int kW = safe_area::kWidth;
  constexpr int kH = safe_area::kHeight;
  constexpr int kR = safe_area::kRadius;
  const bool left = x < kR, right = x > kW - kR;
  const bool top = y < kR, bottom = y > kH - kR;
  if (!((left || right) && (top || bottom))) return true;
  const int cx = left ? kR : kW - kR;
  const int cy = top ? kR : kH - kR;
  const int lim = kR - margin;
  const long long dx = x - cx;
  const long long dy = y - cy;
  return dx * dx + dy * dy <= static_cast<long long>(lim) * lim;
}

// 矩形の4隅すべてが安全域内か。
inline bool rect_in_safe_area(int x, int y, int w, int h,
                              int margin = 6) {
  return point_in_safe_area(x, y, margin) &&
         point_in_safe_area(x + w, y, margin) &&
         point_in_safe_area(x, y + h, margin) &&
         point_in_safe_area(x + w, y + h, margin);
}

}  // namespace watch
