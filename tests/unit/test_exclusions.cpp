/**
 * @file
 *
 * The built-in exclusion band, at the boundaries of its three shapes.
 *
 * ::gfnt_exclusions_band computed in `long double` with `sqrtl`, `floorl` and
 * `ceill` until the change that made it exact integer arithmetic (AD-22: nothing
 * in the rendering path comes from libm, and `long double` is 64 bits of
 * mantissa on x87, 113 on aarch64 Linux and 53 on MSVC, so a result could differ
 * by machine). Every input is an integer (26.6 `int32` coordinates, sizes and
 * margins) and every answer is one, so the arithmetic can be exact: a circle's
 * edge is the ceiling of an integer square root, a polygon edge's crossing is
 * the floor and the ceiling of a rational whose numerator is under 2^64.
 *
 * The expected answers below were recorded from the `long double` version (the
 * commit before the one that made this exact, on x86-64, where `long double` is
 * the x87 type) before it was replaced, so that the change is shown to have
 * changed nothing here. Each case is in tests/exclusion_cases.h:
 * a circle tangent to the band and one unit into it, a radius of nothing, a
 * perfect square under the root and the integers either side, the largest radius
 * and margin the validation accepts, a polygon edge crossing a scan line at an
 * integer, at a fraction of either sign and across the whole int32 range, and
 * every refusal.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstdint>
#include <vector>

#include <ghoti.io/font/layout.h>

#include "../exclusion_cases.h"

namespace {

using gfnttest::ExclusionAnswer;

const ExclusionAnswer kExpected[] = {
    {GFNT_OK, 150, 850, 150},  // circle ordinary, text right
    {GFNT_OK, 0, 50, 150},  // circle ordinary, text left
    {GFNT_OK, 0, 1000, 40},  // circle band ends on the top tangent
    {GFNT_OK, 110, 890, 150},  // circle band one unit into the top
    {GFNT_OK, 0, 1000, 150},  // circle band starts on the bottom tangent
    {GFNT_OK, 110, 890, 150},  // circle band one unit into the bottom
    {GFNT_OK, 150, 850, 150},  // circle centre inside the band
    {GFNT_OK, 100, 900, 100},  // circle zero radius across the centre
    {GFNT_OK, 0, 100, 100},  // circle zero radius, text left
    {GFNT_OK, 0, 1000, 99},  // circle zero radius, band ends at the centre
    {GFNT_OK, 0, 1000, 100},  // circle zero radius, band starts at the centre
    {GFNT_OK, 105, 895, 105},  // circle zero radius, margin only
    {GFNT_OK, 63, 837, 65},  // circle perfect square under the root (16, 63, 65)
    {GFNT_OK, 65, 835, 65},  // circle one under a perfect square
    {GFNT_OK, 2, 898, 2},  // circle one over a perfect square
    {GFNT_OK, 1737, 98263, 2790},  // circle large and non-square
    {GFNT_OK, -10000, 4362, 403},  // circle negative centre, margin
    {GFNT_OK, 46341, 0, 1073741823},  // circle radius reaching the largest dy
    {GFNT_OK, 2147483646, 1, INT32_MAX},  // circle largest radius, centre in the band
    {GFNT_OK, 2147483646, 1, INT32_MAX},  // circle largest radius, dy one
    {GFNT_OK, 0, 1, 1073741823},  // circle large, text left, dy one
    {GFNT_OK, 0, 1, 1073741823},  // circle large, text left, a root a whole unit short of round
    {GFNT_OK, 0, 1073695483, 1073741823},  // circle large, text left, dy near the radius
    {GFNT_OK, INT32_MAX, 0, INT32_MAX},  // circle largest margin on a point
    {GFNT_OK, 0, 0, INT32_MAX},  // circle largest margin, text left
    {GFNT_OK, 2147483646, 0, 2147483646},  // circle radius and margin both large
    {GFNT_OK, 0, 2147481648, 1500},  // circle at the right edge of the range
    {GFNT_OK, -2147481649, 0, 1500},  // circle at the left edge of the range
    {GFNT_ERR_INVALID, 0, 0, 0},  // circle refused: radius past the range
    {GFNT_ERR_INVALID, 0, 0, 0},  // circle refused: negative radius
    {GFNT_ERR_INVALID, 0, 0, 0},  // circle refused: negative margin
    {GFNT_ERR_INVALID, 0, 0, 0},  // circle margin pushes the box out of the range
    {GFNT_OK, 0, 1000, 0},  // circle misses the band
    {GFNT_OK, 367, 1633, 3000},  // polygon triangle, scan through two edges
    {GFNT_OK, -2000, 1633, 3000},  // polygon triangle, text left
    {GFNT_OK, 415, 1585, 3009},  // polygon triangle, margin
    {GFNT_OK, 262, 1738, 400},  // polygon kite, crossings at fractions
    {GFNT_OK, -1000, 938, 400},  // polygon kite, text left
    {GFNT_OK, 400, 1600, 400},  // polygon kite, band through a vertex
    {GFNT_OK, 3, 897, 10},  // polygon fractions, floor and ceiling differ
    {GFNT_OK, -100, 100, 10},  // polygon fractions, text left
    {GFNT_OK, 0, 900, 10},  // polygon fractions, second row
    {GFNT_OK, -50, 400, 100},  // polygon square, scan on an edge
    {GFNT_OK, 100, 250, 100},  // polygon square, band across
    {GFNT_OK, -50, 400, 0},  // polygon square, band above
    {GFNT_OK, -500, 1000, 0},  // polygon all negative
    {GFNT_OK, -500, 1000, 0},  // polygon all negative, text left
    {GFNT_OK, 1000, 2147482647, INT32_MAX},  // polygon across the whole range, slope one
    {GFNT_OK, 0, 0, INT32_MAX},  // polygon across the whole range, a row
    {GFNT_OK, INT32_MAX, 0, INT32_MAX},  // polygon across the whole range, steep edge
    {GFNT_OK, 0, 0, INT32_MAX},  // polygon across the whole range, steep edge, text left
    {GFNT_OK, INT32_MAX, 0, INT32_MAX},  // polygon across the whole range, shallow edge
    {GFNT_OK, 0, 1073741823, INT32_MAX},  // polygon across the whole range, deep scan
    {GFNT_OK, 0, 0, INT32_MAX},  // polygon steep edge, text left, deep scan
    {GFNT_OK, INT32_MAX, 0, INT32_MAX},  // polygon shallow edge, text right, deep scan
    {GFNT_OK, 1825361101, 322122546, INT32_MAX},  // polygon at the edge of the margin's range
    {GFNT_ERR_INVALID, 0, 0, 0},  // polygon refused: margin past the range
    {GFNT_ERR_INVALID, 0, 0, 0},  // polygon refused: two points
    {GFNT_OK, 30, 270, 40},  // rectangle ordinary
    {GFNT_OK, 0, 95, 45},  // rectangle with a margin, text left
    {GFNT_OK, INT32_MAX, 0, INT32_MAX},  // rectangle at the top of the range
    {GFNT_OK, INT32_MIN, 0, 10},  // rectangle at the bottom of the range
    {GFNT_ERR_INVALID, 0, 0, 0},  // rectangle refused: margin past the range
};

}  // namespace

TEST(Exclusions, EveryBoundaryCaseGivesTheAnswerTheLongDoubleVersionGave) {
  const std::vector<gfnttest::ExclusionCase> cases = gfnttest::exclusion_cases();

  ASSERT_EQ(cases.size(), sizeof kExpected / sizeof kExpected[0]);
  for (size_t i = 0; i < cases.size(); ++i) {
    const gfnttest::ExclusionCase & k = cases[i];
    GFNT_Exclusions exclusions{&k.item, 1, k.line_start, k.line_width};
    GFNT_Band band{};
    const GFNT_Result result = gfnt_exclusions_band(&exclusions, k.top, k.height, &band);

    EXPECT_EQ(result, kExpected[i].result) << k.name;
    if (kExpected[i].result == GFNT_OK) {
      EXPECT_EQ(band.start, kExpected[i].start) << k.name;
      EXPECT_EQ(band.width, kExpected[i].width) << k.name;
      EXPECT_EQ(band.next_top, kExpected[i].next_top) << k.name;
    }
  }
}

/**
 * The integer square root under the circle's edge is a ceiling, so the edge is
 * always outside the circle: a point on the edge of the band is never inside the
 * circle. Checked against the definition for a sweep of radii and distances,
 * with the circle's half-width recovered from the band: the half-width `h` the
 * band reports satisfies `(h - 1)^2 < r^2 - dy^2 <= h^2`.
 */
TEST(Exclusions, ACirclesEdgeIsTheCeilingOfItsHalfWidth) {
  for (int32_t radius : {1, 2, 3, 5, 10, 65, 100, 1000, 46340, 46341, 1000000, 1073741823}) {
    for (int64_t dy : {int64_t{0}, int64_t{1}, int64_t{2}, int64_t{radius} / 3,
             int64_t{radius} / 2, int64_t{radius} - 2, int64_t{radius} - 1}) {
      if (dy < 0 || dy >= radius) {
        continue;
      }
      // A circle at x = 1073741824 so its left edge is positive and the line
      // 0..INT32_MAX ends there: the width of the band is the left edge.
      const GFNT_Exclusion item = gfnttest::circle(GFNT_EXCLUSION_TEXT_LEFT, 0, 1073741824, 0, radius);
      GFNT_Exclusions exclusions{&item, 1, 0, INT32_MAX};
      GFNT_Band band{};

      ASSERT_EQ(gfnt_exclusions_band(&exclusions, static_cast<GFNT_F26Dot6>(dy), 0, &band),
          GFNT_OK);
      // All of these are under 2^61, so uint64_t holds them.
      const uint64_t half = static_cast<uint64_t>(1073741824 - band.width);
      const uint64_t n = static_cast<uint64_t>(radius) * static_cast<uint64_t>(radius)
          - static_cast<uint64_t>(dy) * static_cast<uint64_t>(dy);

      EXPECT_LE(n, half * half) << "radius " << radius << " dy " << dy;
      EXPECT_GT(n, (half - 1) * (half - 1)) << "radius " << radius << " dy " << dy;
    }
  }
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
