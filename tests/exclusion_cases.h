/**
 * @file
 *
 * The inputs of the built-in exclusion band's pinned vectors
 * (tests/unit/test_exclusions.cpp).
 *
 * A case is one exclusion, one line (start and width) and one query (top and
 * height). The answers live in the test; the inputs are here so that a program
 * that is not a test can run the same cases against another build of the
 * library and print what it answers.
 *
 * The cases are the boundaries of the three shapes: a circle tangent to the
 * band and just inside it, a radius of zero, a perfect square under the root
 * and the numbers either side of one, the largest radius and margin the
 * validation accepts, negative and huge coordinates; a polygon edge crossing a
 * scan line at an integer, at a fraction (positive and negative, where the
 * floor and the ceiling differ), at a vertex, and on edges that span the whole
 * int32 range, where the product in the crossing is wider than 64 signed bits;
 * a rectangle at the edges of the range.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GFNT_TEST_EXCLUSION_CASES_H
#define GHOTI_IO_GFNT_TEST_EXCLUSION_CASES_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include <ghoti.io/font/font.h>
#include <ghoti.io/font/layout.h>

namespace gfnttest {

/** One query of ::gfnt_exclusions_band against one exclusion. */
struct ExclusionCase {
  const char * name;
  GFNT_Exclusion item;
  GFNT_F26Dot6 line_start;
  GFNT_F26Dot6 line_width;
  GFNT_F26Dot6 top;
  GFNT_F26Dot6 height;
};

/** What the band answers: the result code and the band, which is zero on a refusal. */
struct ExclusionAnswer {
  GFNT_Result result;
  GFNT_F26Dot6 start;
  GFNT_F26Dot6 width;
  GFNT_F26Dot6 next_top;
};

constexpr int32_t I32_MIN = INT32_MIN;
constexpr int32_t I32_MAX = INT32_MAX;

inline GFNT_Exclusion circle(GFNT_ExclusionSide side, GFNT_F26Dot6 margin,
    GFNT_F26Dot6 cx, GFNT_F26Dot6 cy, GFNT_F26Dot6 radius) {
  GFNT_Exclusion item{};

  item.shape = GFNT_EXCLUSION_CIRCLE;
  item.side = side;
  item.margin = margin;
  item.geometry.circle = {{cx, cy}, radius};
  return item;
}

inline GFNT_Exclusion rectangle(GFNT_ExclusionSide side, GFNT_F26Dot6 margin,
    GFNT_F26Dot6 x, GFNT_F26Dot6 y, GFNT_F26Dot6 width, GFNT_F26Dot6 height) {
  GFNT_Exclusion item{};

  item.shape = GFNT_EXCLUSION_RECTANGLE;
  item.side = side;
  item.margin = margin;
  item.geometry.rectangle = {x, y, width, height};
  return item;
}

inline GFNT_Exclusion polygon(GFNT_ExclusionSide side, GFNT_F26Dot6 margin,
    const GFNT_FlowPoint * points, size_t count) {
  GFNT_Exclusion item{};

  item.shape = GFNT_EXCLUSION_POLYGON;
  item.side = side;
  item.margin = margin;
  item.geometry.polygon = {points, count};
  return item;
}

/** The vertices the polygon cases share; static storage, so the pointers stay valid. */
namespace exclusion_points {
const GFNT_FlowPoint triangle[] = {{0, 0}, {1000, 3000}, {-500, 1500}};
const GFNT_FlowPoint kite[] = {{100, 0}, {400, 130}, {100, 400}, {-200, 130}};
const GFNT_FlowPoint fraction[] = {{0, 0}, {7, 3}, {-7, 5}, {0, 10}};
const GFNT_FlowPoint square[] = {{0, 0}, {100, 0}, {100, 100}, {0, 100}};
const GFNT_FlowPoint wide[] = {{I32_MIN, I32_MIN}, {I32_MAX, I32_MAX},
    {I32_MIN, I32_MAX}};
const GFNT_FlowPoint steep[] = {{I32_MIN, 0}, {I32_MAX, 1}, {0, I32_MAX}};
const GFNT_FlowPoint shallow[] = {{I32_MIN, I32_MIN}, {I32_MAX, 5},
    {I32_MAX, I32_MAX}};
const GFNT_FlowPoint negative[] = {{-100, -100}, {-3, -7}, {-31, -90}};
const GFNT_FlowPoint margin_edge[] = {{I32_MIN + 7, 0}, {I32_MAX - 7, 40},
    {0, I32_MAX - 7}};
}  // namespace exclusion_points

inline std::vector<ExclusionCase> exclusion_cases() {
  using namespace exclusion_points;
  const GFNT_ExclusionSide R = GFNT_EXCLUSION_TEXT_RIGHT;
  const GFNT_ExclusionSide L = GFNT_EXCLUSION_TEXT_LEFT;
  std::vector<ExclusionCase> c;
  const auto add = [&c](const char * name, GFNT_Exclusion item,
                       GFNT_F26Dot6 start, GFNT_F26Dot6 width,
                       GFNT_F26Dot6 top, GFNT_F26Dot6 height) {
    c.push_back({name, item, start, width, top, height});
  };

  // --- circles ---------------------------------------------------------
  add("circle ordinary, text right", circle(R, 0, 100, 100, 50), 0, 1000, 90, 10);
  add("circle ordinary, text left", circle(L, 0, 100, 100, 50), 0, 1000, 90, 10);
  add("circle band ends on the top tangent", circle(R, 0, 100, 100, 50), 0, 1000, 40, 10);
  add("circle band one unit into the top", circle(R, 0, 100, 100, 50), 0, 1000, 40, 11);
  add("circle band starts on the bottom tangent", circle(R, 0, 100, 100, 50), 0, 1000, 150, 10);
  add("circle band one unit into the bottom", circle(R, 0, 100, 100, 50), 0, 1000, 149, 10);
  add("circle centre inside the band", circle(R, 0, 100, 100, 50), 0, 1000, 80, 40);
  add("circle zero radius across the centre", circle(R, 0, 100, 100, 0), 0, 1000, 99, 2);
  add("circle zero radius, text left", circle(L, 0, 100, 100, 0), 0, 1000, 99, 2);
  add("circle zero radius, band ends at the centre", circle(R, 0, 100, 100, 0), 0, 1000, 99, 1);
  add("circle zero radius, band starts at the centre", circle(R, 0, 100, 100, 0), 0, 1000, 100, 5);
  add("circle zero radius, margin only", circle(R, 5, 100, 100, 0), 0, 1000, 96, 1);
  add("circle perfect square under the root (16, 63, 65)", circle(R, 0, 0, 0, 65), -100, 1000, 16, 0);
  add("circle one under a perfect square", circle(R, 0, 0, 0, 65), -100, 1000, 8, 0);
  add("circle one over a perfect square", circle(R, 0, 0, 0, 2), -100, 1000, 1, 0);
  add("circle large and non-square", circle(R, 13, 1000, 2000, 777), 0, 100000, 1500, 200);
  add("circle negative centre, margin", circle(L, 3, -5000, -300, 700), -10000, 20000, 0, 64);
  add("circle radius reaching the largest dy", circle(R, 0, 0, 0, 1073741823), 0, 100, 1073741822, 0);
  add("circle largest radius, centre in the band", circle(R, 0, -1, 0, I32_MAX), 0, I32_MAX, 0, 1);
  add("circle largest radius, dy one", circle(R, 0, -1, 0, I32_MAX), 0, I32_MAX, 1, 0);
  add("circle large, text left, dy one", circle(L, 0, 1073741824, 0, 1073741823), 0, I32_MAX, 1, 0);
  add("circle large, text left, a root a whole unit short of round", circle(L, 0, 1073741824, 0, 1073741823), 0, I32_MAX, 40000, 0);
  add("circle large, text left, dy near the radius", circle(L, 0, 1073741824, 0, 1073741823), 0, I32_MAX, 1073741822, 0);
  add("circle largest margin on a point", circle(R, I32_MAX, 0, 0, 0), 0, I32_MAX, 0, 1);
  add("circle largest margin, text left", circle(L, I32_MAX, 0, 0, 0), 0, I32_MAX, 5, 1);
  add("circle radius and margin both large", circle(R, 1073741823, 0, 0, 1073741823), 0, 100, 1073741823, 3);
  add("circle at the right edge of the range", circle(L, 0, I32_MAX - 1000, 500, 1000), 0, I32_MAX, 400, 50);
  add("circle at the left edge of the range", circle(R, 0, I32_MIN + 1000, 500, 1000), I32_MIN, 1000, 400, 50);
  add("circle refused: radius past the range", circle(R, 0, 1, 0, I32_MAX), 0, 100, 0, 1);
  add("circle refused: negative radius", circle(R, 0, 0, 0, -1), 0, 100, 0, 1);
  add("circle refused: negative margin", circle(R, -1, 0, 0, 5), 0, 100, 0, 1);
  add("circle margin pushes the box out of the range", circle(R, 1, I32_MAX - 10, 0, 10), 0, 100, 0, 1);
  add("circle misses the band", circle(R, 0, 100, 1000, 50), 0, 1000, 0, 10);

  // --- polygons --------------------------------------------------------
  add("polygon triangle, scan through two edges", polygon(R, 0, triangle, 3), -2000, 4000, 1000, 100);
  add("polygon triangle, text left", polygon(L, 0, triangle, 3), -2000, 4000, 1000, 100);
  add("polygon triangle, margin", polygon(R, 9, triangle, 3), -2000, 4000, 1200, 7);
  add("polygon kite, crossings at fractions", polygon(R, 0, kite, 4), -1000, 3000, 50, 20);
  add("polygon kite, text left", polygon(L, 0, kite, 4), -1000, 3000, 50, 20);
  add("polygon kite, band through a vertex", polygon(R, 0, kite, 4), -1000, 3000, 130, 0);
  add("polygon fractions, floor and ceiling differ", polygon(R, 0, fraction, 4), -100, 1000, 1, 0);
  add("polygon fractions, text left", polygon(L, 0, fraction, 4), -100, 1000, 1, 0);
  add("polygon fractions, second row", polygon(R, 0, fraction, 4), -100, 1000, 4, 1);
  add("polygon square, scan on an edge", polygon(R, 0, square, 4), -50, 400, 100, 5);
  add("polygon square, band across", polygon(R, 0, square, 4), -50, 400, 20, 30);
  add("polygon square, band above", polygon(R, 0, square, 4), -50, 400, 0, 0);
  add("polygon all negative", polygon(R, 0, negative, 3), -500, 1000, 0, 40);
  add("polygon all negative, text left", polygon(L, 2, negative, 3), -500, 1000, 0, 40);
  add("polygon across the whole range, slope one", polygon(R, 0, wide, 3), 0, I32_MAX, 1000, 0);
  add("polygon across the whole range, a row", polygon(L, 0, wide, 3), 0, I32_MAX, 12345, 100);
  add("polygon across the whole range, steep edge", polygon(R, 0, steep, 3), 0, I32_MAX, 1, 0);
  add("polygon across the whole range, steep edge, text left", polygon(L, 0, steep, 3), 0, I32_MAX, 7, 3);
  add("polygon across the whole range, shallow edge", polygon(R, 0, shallow, 3), 0, I32_MAX, 9, 0);
  add("polygon across the whole range, deep scan", polygon(L, 0, shallow, 3), 0, I32_MAX, 1073741823, 1000);
  add("polygon steep edge, text left, deep scan", polygon(L, 0, steep, 3), 0, I32_MAX, 1073741823, 3);
  add("polygon shallow edge, text right, deep scan", polygon(R, 0, shallow, 3), 0, I32_MAX, 1073741823, 3);
  add("polygon at the edge of the margin's range", polygon(R, 7, margin_edge, 3), 0, I32_MAX, 20, 10);
  add("polygon refused: margin past the range", polygon(R, 8, margin_edge, 3), 0, I32_MAX, 20, 10);
  add("polygon refused: two points", polygon(R, 0, triangle, 2), 0, 100, 0, 1);

  // --- rectangles ------------------------------------------------------
  add("rectangle ordinary", rectangle(R, 0, 10, 10, 20, 30), 0, 300, 15, 10);
  add("rectangle with a margin, text left", rectangle(L, 5, 100, 10, 20, 30), 0, 300, 15, 10);
  add("rectangle at the top of the range", rectangle(R, 0, I32_MAX - 10, I32_MAX - 10, 10, 10), 0, I32_MAX, I32_MAX - 5, 2);
  add("rectangle at the bottom of the range", rectangle(L, 0, I32_MIN, 0, 10, 10), I32_MIN, I32_MAX, 2, 2);
  add("rectangle refused: margin past the range", rectangle(R, 1, I32_MAX - 10, 0, 10, 10), 0, 100, 0, 1);
  return c;
}

}  // namespace gfnttest

#endif
