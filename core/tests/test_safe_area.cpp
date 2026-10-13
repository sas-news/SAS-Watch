// test_safe_area.cpp — 丸角パネルの安全域判定 (safe_area.hpp)。
#include <gtest/gtest.h>

#include "watch/safe_area.hpp"

namespace {

// 画面中央は常に安全。
TEST(SafeArea, CenterRect) {
  EXPECT_TRUE(watch::rect_in_safe_area(100, 100, 200, 200));
}

// 中央列の帯 (x が角の外側) は上下端まで伸びても制約されない。
TEST(SafeArea, CenterColumnFullHeight) {
  EXPECT_TRUE(watch::rect_in_safe_area(100, 0, 210, 502));
}

// 角にべったりの矩形は内側に入っていないので NG。
TEST(SafeArea, CornerRectFails) {
  EXPECT_FALSE(watch::rect_in_safe_area(0, 0, 10, 10));
  EXPECT_FALSE(watch::rect_in_safe_area(400, 0, 10, 10));
  EXPECT_FALSE(watch::rect_in_safe_area(0, 492, 10, 10));
  EXPECT_FALSE(watch::rect_in_safe_area(400, 492, 10, 10));
}

// 角帯内でも円弧の内側なら OK。(50,50): dx=50,dy=50 → 5000 <= 94^2。
TEST(SafeArea, InsideArc) {
  EXPECT_TRUE(watch::rect_in_safe_area(50, 50, 1, 1));
  EXPECT_TRUE(watch::rect_in_safe_area(40, 40, 1, 1));
  EXPECT_TRUE(watch::rect_in_safe_area(36, 36, 1, 1));
}

// 円弧の外側なら NG。(30,30): dx=70,dy=70 → 9800 > 94^2。
TEST(SafeArea, OutsideArcFails) {
  EXPECT_FALSE(watch::rect_in_safe_area(30, 30, 1, 1));
  EXPECT_FALSE(watch::rect_in_safe_area(20, 20, 1, 1));
}

// 角帯の外側 (x>=R または y>=R) では角円は無関係。
TEST(SafeArea, StraightEdgeOk) {
  EXPECT_TRUE(watch::rect_in_safe_area(0, 100, 20, 20));   // 左辺の直線部
  EXPECT_TRUE(watch::rect_in_safe_area(100, 0, 20, 20));   // 上辺の直線部
  EXPECT_TRUE(watch::rect_in_safe_area(390, 100, 20, 20)); // 右辺
  EXPECT_TRUE(watch::rect_in_safe_area(100, 482, 20, 20)); // 下辺
}

// 右下コーナーの対称性を確認 (右上の角点が制約を受ける)。
TEST(SafeArea, BottomRightCorner) {
  // (380,472): 円中心(310,402) から dx=70,dy=70 → 9800 > 94^2 → NG。
  EXPECT_FALSE(watch::rect_in_safe_area(380, 472, 1, 1));
  // (360,452): dx=50,dy=50 → 5000 <= 94^2 → OK。
  EXPECT_TRUE(watch::rect_in_safe_area(360, 452, 1, 1));
}

// margin を大きくすると同じ点でも NG になる。
TEST(SafeArea, MarginEffect) {
  // (36,36) は margin=6 で OK。margin=20 なら lim=80 → 8192 > 6400 で NG。
  EXPECT_TRUE(watch::rect_in_safe_area(36, 36, 1, 1, 6));
  EXPECT_FALSE(watch::rect_in_safe_area(36, 36, 1, 1, 20));
}

// 境界値: margin=0 では円周上が境界。(0,100) は直線部なので無関係。
// (10,90): dx=90,dy=10 → 8200 <= 100^2=10000 → margin=0 では OK、
// margin=6 では 8200 <= 8836 → 依然 OK。(14,90): dx=86,dy=10 → 7496 OK。
// 厳しい境界点: (29,28): dx=71,dy=72 → 5041+5184=10225 > 8836 → NG、
// margin=0 でも 10225 > 10000 → NG。
TEST(SafeArea, BoundaryPoints) {
  EXPECT_TRUE(watch::rect_in_safe_area(14, 90, 1, 1));
  EXPECT_FALSE(watch::rect_in_safe_area(29, 28, 1, 1, 0));
  EXPECT_FALSE(watch::rect_in_safe_area(29, 28, 1, 1, 6));
}

}  // namespace
