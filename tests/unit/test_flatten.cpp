/**
 * @file
 *
 * The flattener as `font` uses it: the glue, since the flattener itself is
 * `shape-geometry`'s.
 *
 * This library kept a copy of the flattener (`src/raster/flatten.c`,
 * with its proof and a test that measured the polyline against the curve) until
 * the cleanup that followed story 9 of the shape family's first milestone. The
 * copy is gone; the proof sits in `shape-geometry`'s `src/flatten/flatten.c`
 * and the measurement, at every tolerance this library accepts, in its
 * `tests/unit/test_tolerance.cpp`. What belongs here is what only this library
 * can say:
 *
 *  - the revision and the smallest tolerance that `raster.h` states are the
 *    flattener's own;
 *  - the option `GFNT_RasterOptions::tolerance` reaches the flattener: the
 *    renderings at ten tolerances are what they were when this library's own
 *    copy produced them (pinned from the commit before the flattener moved, for
 *    both fill rules, with a sub-pixel origin), so the move changed no pixel and
 *    a tolerance that stopped being passed on would change a hash;
 *  - the refusal of a tolerance the flattener cannot keep is this library's
 *    (`GFNT_ERR_INVALID`), and a tolerance it can is accepted up to INT32_MAX.
 *
 * documentation/design.md section 8.1.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstdint>

#include <ghoti.io/font/outline.h>
#include <ghoti.io/font/raster.h>
#include <ghoti.io/shape-geometry/flatten.h>

namespace {

/**
 * Three contours of curves large enough that every tolerance changes the
 * polyline: a quadratic blob, a cubic loop, and a shallow quadratic far from the
 * origin in negative coordinates. 26.6 units, so up to 300 pixels across.
 */
GFNT_Outline * curved_outline() {
  struct Point {
    int32_t x;
    int32_t y;
    GFNT_PointTag tag;
  };
  static const Point blob[] = {
      {640, 640, GFNT_POINT_ON}, {640, 9000, GFNT_POINT_QUAD},
      {4800, 9000, GFNT_POINT_ON}, {9000, 9000, GFNT_POINT_QUAD},
      {9000, 4800, GFNT_POINT_ON}, {9000, 300, GFNT_POINT_QUAD},
      {5000, 640, GFNT_POINT_ON}, {2000, -1500, GFNT_POINT_QUAD}};
  static const Point loop[] = {
      {12000, 500, GFNT_POINT_ON}, {20000, 9000, GFNT_POINT_CUBIC},
      {9000, 9000, GFNT_POINT_CUBIC}, {15000, 500, GFNT_POINT_ON},
      {14000, -3000, GFNT_POINT_CUBIC}, {12500, -3000, GFNT_POINT_CUBIC}};
  static const Point shallow[] = {
      {-8000, -7000, GFNT_POINT_ON}, {-3000, -9000, GFNT_POINT_QUAD},
      {-300, -6000, GFNT_POINT_ON}, {-4000, -4500, GFNT_POINT_QUAD}};
  struct Contour {
    const Point * points;
    size_t count;
  };
  const Contour contours[] = {{blob, 8}, {loop, 6}, {shallow, 4}};
  GFNT_Outline * outline = nullptr;
  GFNT_Error error{};

  if (gfnt_outline_create(nullptr, &outline, &error) != GFNT_OK) {
    return nullptr;
  }
  for (const Contour & contour : contours) {
    if (gfnt_outline_begin_contour(outline, &error) != GFNT_OK) {
      return nullptr;
    }
    for (size_t i = 0; i < contour.count; ++i) {
      const Point & point = contour.points[i];

      if (gfnt_outline_add_point(outline, GFNT_Point{point.x, point.y}, point.tag,
              &error) != GFNT_OK) {
        return nullptr;
      }
    }
  }
  if (gfnt_outline_scale(outline, GFNT_F16DOT16_ONE, &error) != GFNT_OK) {
    return nullptr;
  }
  return outline;
}

}  // namespace

TEST(Flatten, TheRevisionAndTheSmallestToleranceAreTheFlattenersOwn) {
  EXPECT_EQ(GFNT_RASTER_FLATTEN_REVISION, 1);
  EXPECT_EQ(GFNT_RASTER_MIN_TOLERANCE, 2);
  // Equal at compile time (raster.c); equal at run time too, which is what
  // matters when the library found at run time is not the one compiled against.
  EXPECT_EQ(GFNT_RASTER_FLATTEN_REVISION, gshgeom_flatten_revision());
  EXPECT_EQ(GFNT_RASTER_MIN_TOLERANCE, GSHGEOM_FLATTEN_MIN_TOLERANCE);
}

/**
 * The renderings of one outline at ten tolerances, both fill rules, with a
 * sub-pixel origin, as this library's own flattener produced them at commit
 * bbd9a47 (the parent of the one that removed that flattener). Zero is the
 * default and so renders as 4; 2 and 3 are the smallest the option takes;
 * 2147483647 is the largest, a chord for almost every curve.
 */
TEST(Flatten, TheToleranceOptionReachesTheFlattenerAndTheRenderingsAreWhatTheyWere) {
  struct Known {
    GFNT_FillRule fill;
    int32_t tolerance;
    uint32_t width;
    uint32_t height;
    int32_t left;
    int32_t top;
    uint64_t total;
    uint64_t hash;
  };
  const Known known[] = {
      {GFNT_FILL_NONZERO, 0, 361, 264, -125, 141, 5470217, 0x44024C2BB7B9737DULL},
      {GFNT_FILL_NONZERO, 2, 361, 264, -125, 141, 5471799, 0x1275FAC6B347D8C7ULL},
      {GFNT_FILL_NONZERO, 3, 361, 264, -125, 141, 5471137, 0x811951F4B7E24BF7ULL},
      {GFNT_FILL_NONZERO, 4, 361, 264, -125, 141, 5470217, 0x44024C2BB7B9737DULL},
      {GFNT_FILL_NONZERO, 5, 361, 264, -125, 141, 5469348, 0xB533D9750113AE8EULL},
      {GFNT_FILL_NONZERO, 8, 361, 264, -125, 141, 5467949, 0xEFEF42ADE1CB6635ULL},
      {GFNT_FILL_NONZERO, 16, 361, 264, -125, 141, 5460116, 0xCF036519F92177F0ULL},
      {GFNT_FILL_NONZERO, 64, 361, 264, -125, 141, 5421938, 0xB096128B5D1B079EULL},
      {GFNT_FILL_NONZERO, 1000, 360, 263, -125, 141, 4718676, 0xC924A8537851651AULL},
      {GFNT_FILL_NONZERO, INT32_MAX, 251, 249, -110, 141, 2201829, 0x4BA7D214938090DBULL},
      {GFNT_FILL_EVEN_ODD, 0, 361, 264, -125, 141, 5470217, 0xD054F60C7ED855D8ULL},
      {GFNT_FILL_EVEN_ODD, 2, 361, 264, -125, 141, 5471799, 0xEC1E99BBFFE1F57EULL},
      {GFNT_FILL_EVEN_ODD, 3, 361, 264, -125, 141, 5471137, 0xEC2F2C65F6C5FBDEULL},
      {GFNT_FILL_EVEN_ODD, 4, 361, 264, -125, 141, 5470217, 0xD054F60C7ED855D8ULL},
      {GFNT_FILL_EVEN_ODD, 5, 361, 264, -125, 141, 5469348, 0x38F1D334195620C3ULL},
      {GFNT_FILL_EVEN_ODD, 8, 361, 264, -125, 141, 5467949, 0x5BB9B338F5AD3644ULL},
      {GFNT_FILL_EVEN_ODD, 16, 361, 264, -125, 141, 5460116, 0x55F3D571C6B2D9B1ULL},
      {GFNT_FILL_EVEN_ODD, 64, 361, 264, -125, 141, 5421938, 0xC57BF996288A2E63ULL},
      {GFNT_FILL_EVEN_ODD, 1000, 360, 263, -125, 141, 4718676, 0x450DBDCC9E2F4987ULL},
      {GFNT_FILL_EVEN_ODD, INT32_MAX, 251, 249, -110, 141, 2201829, 0xF1BC7F5BB11895CCULL},
  };
  GFNT_Outline * outline = curved_outline();

  ASSERT_NE(outline, nullptr);
  for (const Known & k : known) {
    GFNT_RasterOptions options{};
    GFNT_Coverage coverage{};
    GFNT_Error error{};

    options.fill = k.fill;
    options.tolerance = k.tolerance;
    options.origin_x = 19;
    options.origin_y = -23;
    ASSERT_EQ(gfnt_raster_outline(outline, &options, nullptr, nullptr, &coverage, &error),
        GFNT_OK) << "tolerance " << k.tolerance;
    EXPECT_EQ(coverage.width, k.width) << "tolerance " << k.tolerance;
    EXPECT_EQ(coverage.height, k.height) << "tolerance " << k.tolerance;
    EXPECT_EQ(coverage.left, k.left) << "tolerance " << k.tolerance;
    EXPECT_EQ(coverage.top, k.top) << "tolerance " << k.tolerance;
    EXPECT_EQ(gfnt_coverage_total(&coverage), k.total) << "tolerance " << k.tolerance;
    EXPECT_EQ(gfnt_coverage_hash(&coverage), k.hash) << "tolerance " << k.tolerance
        << " fill " << static_cast<int>(k.fill);
    gfnt_coverage_destroy(&coverage);
  }
  gfnt_outline_destroy(outline);
}

/** The public door: the option is a promise, so a promise it cannot keep is refused. */
TEST(Flatten, TheRasteriserRefusesATolerancePastWhatItCanKeep) {
  GFNT_Outline * outline = nullptr;
  GFNT_Error error{};
  GFNT_Coverage coverage{};
  GFNT_RasterOptions options{};

  ASSERT_EQ(gfnt_outline_create(nullptr, &outline, &error), GFNT_OK);
  const GFNT_Point square[] = {{0, 0}, {64 * 8, 0}, {64 * 8, 64 * 8}, {0, 64 * 8}};
  ASSERT_EQ(gfnt_outline_begin_contour(outline, &error), GFNT_OK);
  for (const GFNT_Point & point : square) {
    ASSERT_EQ(gfnt_outline_add_point(outline, point, GFNT_POINT_ON, &error), GFNT_OK);
  }
  ASSERT_EQ(gfnt_outline_scale(outline, GFNT_F16DOT16_ONE, &error), GFNT_OK);

  for (const GFNT_F26Dot6 refused : {GFNT_F26Dot6{INT32_MIN}, GFNT_F26Dot6{-1}, GFNT_F26Dot6{1}}) {
    options.tolerance = refused;
    EXPECT_EQ(gfnt_raster_outline(outline, &options, nullptr, nullptr, &coverage, &error),
        GFNT_ERR_INVALID) << "tolerance " << refused;
  }
  for (const GFNT_F26Dot6 accepted : {GFNT_F26Dot6{0}, GFNT_F26Dot6{2}, GFNT_F26Dot6{4},
           GFNT_F26Dot6{INT32_MAX}}) {
    options.tolerance = accepted;
    EXPECT_EQ(gfnt_raster_outline(outline, &options, nullptr, nullptr, &coverage, &error),
        GFNT_OK) << "tolerance " << accepted;
    gfnt_coverage_destroy(&coverage);
  }
  gfnt_outline_destroy(outline);
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
