/**
 * @file
 *
 * The one unhinted rounding (story 14b of the shape family's first milestone,
 * D1; src/scene/unhinted.c), pinned at its ties.
 *
 * A 2048-unit face at 16 ppem has a scale of exactly 512/65536 per unit, so
 * every odd design unit lands on half a 26.6 unit: a tie. D1 decomposes in
 * design units and rounds each emitted point once, half up **in y down**
 * (`floor(x * 64 + 0.5)` of the device coordinate). The rounding it replaced
 * scaled first, ties away from zero in y up, and took `glyf`'s implied
 * midpoints after, floored. The two disagree:
 *
 * - above the baseline (y up positive): a tie goes down, not up;
 * - left of the origin (x negative): a tie goes right, not left;
 * - at an implied midpoint, which is now taken exactly in design units and
 *   rounded once, instead of from two already-rounded ends.
 *
 * The expected values are written out here, worked by hand from the
 * definition, not computed by the code under test. Each case also states what
 * the previous rounding gave, and the renderer is seen to draw the D1 shape and
 * not that one.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "sfnt_builder.h"

#include <string>
#include <vector>

#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/outline.h>
#include <ghoti.io/font/raster.h>
#include <ghoti.io/shape-geometry/device.h>

#include "../../src/scene/unhinted.h"

namespace {

/**
 * A 2048-unit TrueType face with no program. Glyph 1 is a rectangle whose
 * corners are odd units left of the origin and above the baseline; glyph 2 is a
 * quadratic contour with two off-curve points in a row (an implied midpoint).
 */
std::vector<uint8_t> tie_font() {
  using gfnttest::GlyfPoint;
  std::vector<std::vector<uint8_t>> glyphs = {
      {},
      gfnttest::build_glyf_glyph({{{-3, 3, true}, {1021, 3, true},
          {1021, 1501, true}, {-3, 1501, true}}}),
      gfnttest::build_glyf_glyph({{{0, 0, true}, {202, 101, false}, {606, 505, false},
          {808, 0, true}}}),
  };
  std::vector<uint8_t> glyf;
  std::vector<uint8_t> loca;
  bool long_loca = false;
  gfnttest::build_glyf_and_loca(glyphs, &glyf, &loca, &long_loca);
  return gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, {
      {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(2048, long_loca ? 1 : 0)},
      {GFNT_TAG('h', 'h', 'e', 'a'), gfnttest::build_hhea(1800, -400, 0, 3)},
      {GFNT_TAG('h', 'm', 't', 'x'),
          gfnttest::build_hmtx({{1024, 0}, {1031, -3}, {1025, 0}}, {})},
      {GFNT_TAG('m', 'a', 'x', 'p'), gfnttest::build_maxp(3)},
      {GFNT_TAG('g', 'l', 'y', 'f'), glyf},
      {GFNT_TAG('l', 'o', 'c', 'a'), loca},
  });
}

/** The tie font, loaded. */
struct TieFace {
  std::vector<uint8_t> bytes = tie_font();
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Error error{};

  TieFace() {
    EXPECT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(),
        GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, &error), GFNT_OK);
    EXPECT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error), GFNT_OK);
  }
  ~TieFace() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
  TieFace(const TieFace &) = delete;
  TieFace & operator=(const TieFace &) = delete;
};

/** A point as `x,y`. */
std::string pt(GFNT_Point p) {
  return std::to_string(p.x) + "," + std::to_string(p.y);
}

/** A path as one string per segment: 26.6 coordinates, as given. */
std::vector<std::string> segments(const GFNT_Outline * outline) {
  using Lines = std::vector<std::string>;
  Lines out;
  GFNT_OutlineSink sink{};
  sink.move_to = [](void * u, GFNT_Point to) -> GFNT_Result {
    static_cast<Lines *>(u)->push_back("M " + pt(to));
    return GFNT_OK;
  };
  sink.line_to = [](void * u, GFNT_Point to) -> GFNT_Result {
    static_cast<Lines *>(u)->push_back("L " + pt(to));
    return GFNT_OK;
  };
  sink.quad_to = [](void * u, GFNT_Point c, GFNT_Point to) -> GFNT_Result {
    static_cast<Lines *>(u)->push_back("Q " + pt(c) + " " + pt(to));
    return GFNT_OK;
  };
  sink.cubic_to = [](void * u, GFNT_Point a, GFNT_Point b, GFNT_Point to) -> GFNT_Result {
    static_cast<Lines *>(u)->push_back("C " + pt(a) + " " + pt(b) + " " + pt(to));
    return GFNT_OK;
  };
  sink.close = [](void * u) -> GFNT_Result {
    static_cast<Lines *>(u)->push_back("Z");
    return GFNT_OK;
  };
  EXPECT_EQ(gfnt_outline_decompose(outline, &sink, &out, nullptr), GFNT_OK);
  return out;
}

/** The D1 pixels of a glyph of the tie face at 16 ppem, as segments. */
std::vector<std::string> d1_segments(const GFNT_Face * face, uint32_t glyph) {
  GFNT_Outline * units = nullptr;
  GFNT_Outline * pixels = nullptr;
  EXPECT_EQ(gfnt_face_glyph_outline(face, glyph, nullptr, nullptr, &units, nullptr), GFNT_OK);
  EXPECT_EQ(gfnt_unhinted_pixels(units, gfnt_scale_for_ppem(2048, 16), nullptr, &pixels, nullptr),
      GFNT_OK);
  std::vector<std::string> out = pixels ? segments(pixels) : std::vector<std::string>{};
  EXPECT_EQ(gfnt_outline_space(pixels), GFNT_OUTLINE_PIXELS);
  gfnt_outline_destroy(pixels);
  gfnt_outline_destroy(units);
  return out;
}

/** A coverage hash of a hand-made pixel outline (26.6, y up), contour by contour. */
uint64_t hash_of(const std::vector<std::vector<GFNT_Point>> & contours,
    const std::vector<std::vector<GFNT_PointTag>> & tags) {
  GFNT_Outline * outline = nullptr;
  EXPECT_EQ(gfnt_outline_create(nullptr, &outline, nullptr), GFNT_OK);
  for (size_t c = 0; c < contours.size(); ++c) {
    EXPECT_EQ(gfnt_outline_begin_contour(outline, nullptr), GFNT_OK);
    for (size_t i = 0; i < contours[c].size(); ++i) {
      EXPECT_EQ(gfnt_outline_add_point(outline, contours[c][i], tags[c][i], nullptr), GFNT_OK);
    }
  }
  // One 26.6 unit per unit of an em of 64: the numbers above are the pixels.
  EXPECT_EQ(gfnt_outline_scale(outline, GFNT_F16DOT16_ONE, nullptr), GFNT_OK);
  GFNT_Coverage coverage{};
  EXPECT_EQ(gfnt_raster_outline(outline, nullptr, nullptr, nullptr, &coverage, nullptr), GFNT_OK);
  uint64_t hash = gfnt_coverage_hash(&coverage);
  gfnt_coverage_destroy(&coverage);
  gfnt_outline_destroy(outline);
  return hash;
}

uint64_t render_hash(const GFNT_Face * face, uint32_t glyph, GFNT_HintMode mode) {
  GFNT_RasterOptions options{};
  options.hint_mode = mode;
  GFNT_Coverage coverage{};
  GFNT_Error error{};
  EXPECT_EQ(gfnt_face_render_glyph(face, glyph, 16, &options, nullptr, &coverage, &error), GFNT_OK)
      << (error.message ? error.message : "");
  uint64_t hash = gfnt_coverage_hash(&coverage);
  gfnt_coverage_destroy(&coverage);
  return hash;
}

const std::vector<std::vector<GFNT_PointTag>> kRectTags = {
    {GFNT_POINT_ON, GFNT_POINT_ON, GFNT_POINT_ON, GFNT_POINT_ON}};

}  // namespace

TEST(Unhinted, TheScaleOfA2048UnitFaceAt16PpemMakesEveryOddUnitATie) {
  EXPECT_EQ(gfnt_scale_for_ppem(2048, 16), 512);
  int32_t out = 0;
  // 3 units is 192 in 26.6 units, 192 * 512 / 65536 = 1.5 26.6 pixels.
  ASSERT_EQ(gshgeom_scale_26_6(3 * 64, 512, 0, &out), GSHGEOM_OK);
  EXPECT_EQ(out, 2) << "x: half up";
  ASSERT_EQ(gshgeom_scale_26_6(-3 * 64, 512, 0, &out), GSHGEOM_OK);
  EXPECT_EQ(out, -1) << "x left of the origin: half up, so towards the origin";
  ASSERT_EQ(gshgeom_scale_26_6(3 * 64, 512, 1, &out), GSHGEOM_OK);
  EXPECT_EQ(out, -1) << "y above the baseline: -1.5 in y down, half up";
}

// Corners at odd units left of the origin and above the baseline: each a tie.
TEST(Unhinted, TiesAboveTheBaselineAndLeftOfTheOriginRoundHalfUpInYDown) {
  TieFace f;
  ASSERT_NE(f.face, nullptr);
  // x: -3 -> -1.5 -> -1; 1021 -> 510.5 -> 511.  y (up): 3 -> 1.5 -> -1.5 down
  // -> -1 -> 1 up; 1501 -> 750.5 -> 750.  The previous rounding gave -2, 511,
  // 2 and 751: every tie but the positive x one moved.
  const std::vector<std::string> expected = {
      "M -1,1", "L 511,1", "L 511,750", "L -1,750", "Z"};
  EXPECT_EQ(d1_segments(f.face, 1), expected);
}

// glyf's implied midpoint is taken in design units, then rounded once.
TEST(Unhinted, AnImpliedMidpointIsTakenInDesignUnitsAndRoundedOnce) {
  TieFace f;
  ASSERT_NE(f.face, nullptr);
  // Off-curve (202,101) and (606,505) imply (404,303).  At 1/2 per unit (in
  // 26.6 pixels): (101,50.5) -> (101,50), (303,252.5) -> (303,252), the
  // midpoint (202,151.5) -> (202,151), (404,0).  The previous rounding took the
  // ends to (101,51) and (303,253) and floored their mean: (202,152).
  const std::vector<std::string> expected = {
      "M 0,0", "Q 101,50 202,151", "Q 303,252 404,0", "Z"};
  EXPECT_EQ(d1_segments(f.face, 2), expected);
}

// The device path the provider gives: the same points, y down, moved by the fraction.
TEST(Unhinted, TheDevicePathIsTheSamePointsYDownPlusTheFraction) {
  TieFace f;
  ASSERT_NE(f.face, nullptr);
  GFNT_Outline * units = nullptr;
  ASSERT_EQ(gfnt_face_glyph_outline(f.face, 1, nullptr, nullptr, &units, nullptr), GFNT_OK);
  GSHGEOM_Device_Path * path = nullptr;
  ASSERT_EQ(gshgeom_device_path_create(nullptr, nullptr, &path), GSHGEOM_OK);
  ASSERT_EQ(gfnt_unhinted_device_path(units, 512, 16, 33, path, nullptr), GFNT_OK);
  const int32_t want[4][2] = {{15, 32}, {527, 32}, {527, -717}, {15, -717}};
  ASSERT_EQ(gshgeom_device_path_element_count(path), 5u);
  for (size_t i = 0; i < 4; ++i) {
    GSHGEOM_Device_Element e{};
    ASSERT_EQ(gshgeom_device_path_element(path, i, &e), GSHGEOM_OK);
    EXPECT_EQ(e.p[0].x, want[i][0]) << i;
    EXPECT_EQ(e.p[0].y, want[i][1]) << i;
  }
  gshgeom_device_path_destroy(path);
  gfnt_outline_destroy(units);
}

// The direct renderer draws D1's shape, not the previous rounding's, in every mode
// that leaves the glyph unhinted (this face has no program, so FONT is unhinted too).
TEST(Unhinted, TheRendererDrawsTheOneRoundingAndNotThePreviousOne) {
  TieFace f;
  ASSERT_NE(f.face, nullptr);
  const uint64_t d1 = hash_of({{{-1, 1}, {511, 1}, {511, 750}, {-1, 750}}}, kRectTags);
  const uint64_t before = hash_of({{{-2, 2}, {511, 2}, {511, 751}, {-2, 751}}}, kRectTags);
  ASSERT_NE(d1, before) << "the two roundings must draw differently, or this says nothing";
  EXPECT_EQ(render_hash(f.face, 1, GFNT_HINT_MODE_NONE), d1);
  EXPECT_EQ(render_hash(f.face, 1, GFNT_HINT_MODE_FONT), d1)
      << "a glyph the hinter returns unhinted takes the same rounding";
  const uint64_t mid = hash_of({{{0, 0}, {101, 50}, {202, 151}, {303, 252}, {404, 0}}},
      {{GFNT_POINT_ON, GFNT_POINT_QUAD, GFNT_POINT_ON, GFNT_POINT_QUAD, GFNT_POINT_ON}});
  const uint64_t mid_before = hash_of({{{0, 0}, {101, 51}, {202, 152}, {303, 253}, {404, 0}}},
      {{GFNT_POINT_ON, GFNT_POINT_QUAD, GFNT_POINT_ON, GFNT_POINT_QUAD, GFNT_POINT_ON}});
  ASSERT_NE(mid, mid_before);
  EXPECT_EQ(render_hash(f.face, 2, GFNT_HINT_MODE_NONE), mid);
}

// The walk refuses an outline already in pixels, and a point past 26.6.
TEST(Unhinted, TheWalkTakesDesignUnitsAndRefusesAPointPast26Dot6) {
  GFNT_Outline * outline = nullptr;
  ASSERT_EQ(gfnt_outline_create(nullptr, &outline, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_outline_begin_contour(outline, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_outline_add_point(outline, {INT32_MAX, 0}, GFNT_POINT_ON, nullptr), GFNT_OK);
  GFNT_Outline * pixels = nullptr;
  GFNT_Error error{};
  // 2^31 / 64 units at 16.16's largest scale is far past 26.6.
  EXPECT_EQ(gfnt_unhinted_pixels(outline, INT32_MAX, nullptr, &pixels, &error), GFNT_ERR_LIMIT);
  EXPECT_EQ(pixels, nullptr);
  GSHGEOM_Device_Path * path = nullptr;
  ASSERT_EQ(gshgeom_device_path_create(nullptr, nullptr, &path), GSHGEOM_OK);
  // A fraction that carries a point past the range at a scale of one.
  ASSERT_EQ(gfnt_outline_scale(outline, GFNT_F16DOT16_ONE, nullptr), GFNT_OK);
  EXPECT_EQ(gfnt_unhinted_device_path(outline, 65536, 63, 0, path, nullptr), GFNT_ERR_INVALID)
      << "an outline in pixels is not the walk's";
  gshgeom_device_path_destroy(path);
  gfnt_outline_destroy(outline);
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
