/*
 * Tests for the public hinting API: `gasp`, contexts, hinted outlines and the
 * hint mode of ::gfnt_face_render_glyph().
 *
 * The glyph is a square whose program pulls its bottom edge from 13 to 0 and its
 * top edge from 213 to 192 (in 26.6 pixels), so hinted it is three rows tall and
 * as drawn it is four.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "hint_fixture.h"

#include <ghoti.io/font/raster.h>

namespace {

Bytes hinting_program() {
  return cat({op({0x00}), push({0}), op({0x2F}), push({1}), op({0x2F}), push({2}),
      op({0x2F}), push({3}), op({0x2F})});
}

Bytes gasp(std::initializer_list<std::pair<int, int>> ranges) {
  Bytes out{0, 1, 0, static_cast<uint8_t>(ranges.size())};
  for (auto r : ranges) {
    out.push_back(static_cast<uint8_t>(r.first >> 8));
    out.push_back(static_cast<uint8_t>(r.first & 0xFF));
    out.push_back(static_cast<uint8_t>(r.second >> 8));
    out.push_back(static_cast<uint8_t>(r.second & 0xFF));
  }
  return out;
}

/** The rows a glyph's coverage has, or 0 for a refusal. */
uint32_t rows(const Fixture & f, GFNT_HintMode mode, GFNT_Result * result = nullptr,
    GFNT_HintVersion version = GFNT_HINT_VERSION_AUTO) {
  GFNT_RasterOptions options{};
  GFNT_Coverage coverage{};
  GFNT_Error error{};
  uint32_t height = 0;
  GFNT_Result r;

  options.hint_mode = mode;
  options.hint_version = version;
  r = gfnt_face_render_glyph(f.face, 0, 10, &options, nullptr, &coverage, &error);
  if (r == GFNT_OK) {
    height = coverage.height;
    gfnt_coverage_destroy(&coverage);
  }
  if (result) *result = r;
  return height;
}

Fixture square(const Bytes & program, const Bytes & table = {}) {
  return Fixture({gfnttest::build_glyf_glyph({kSquare}, program)}, {{300, 40}}, {}, 35, 10,
      table);
}

}  // namespace

TEST(HintApi, TheFontsOwnProgramIsTheDefault) {
  Fixture f({gfnttest::build_glyf_glyph({kSquare}, hinting_program())}, {{300, 40}});
  GFNT_RasterOptions zero{};

  EXPECT_EQ(zero.hint_mode, GFNT_HINT_MODE_FONT);
  EXPECT_EQ(rows(f, GFNT_HINT_MODE_FONT), 3u);
  EXPECT_EQ(rows(f, GFNT_HINT_MODE_NONE), 4u);
}

TEST(HintApi, ANullOptionsPointerHintsToo) {
  Fixture f({gfnttest::build_glyf_glyph({kSquare}, hinting_program())}, {{300, 40}});
  GFNT_Coverage coverage{};

  ASSERT_EQ(gfnt_face_render_glyph(f.face, 0, 10, nullptr, nullptr, &coverage, nullptr),
      GFNT_OK);
  EXPECT_EQ(coverage.height, 3u);
  gfnt_coverage_destroy(&coverage);
}

TEST(HintApi, EitherVersionRendersAndTheAutoVersionIsForty) {
  Fixture f({gfnttest::build_glyf_glyph({kSquare}, hinting_program())}, {{300, 40}});

  EXPECT_EQ(rows(f, GFNT_HINT_MODE_FONT, nullptr, GFNT_HINT_VERSION_35), 3u);
  EXPECT_EQ(rows(f, GFNT_HINT_MODE_FONT, nullptr, GFNT_HINT_VERSION_40), 3u);
  EXPECT_EQ(rows(f, GFNT_HINT_MODE_FONT, nullptr, GFNT_HINT_VERSION_AUTO), 3u);
}

TEST(HintApi, AutoHintingIsReservedAndAnUnknownModeIsInvalid) {
  Fixture f({gfnttest::build_glyf_glyph({kSquare})}, {{300, 40}});
  GFNT_Result r;

  rows(f, GFNT_HINT_MODE_AUTO, &r);
  EXPECT_EQ(r, GFNT_ERR_UNSUPPORTED);
  rows(f, static_cast<GFNT_HintMode>(99), &r);
  EXPECT_EQ(r, GFNT_ERR_INVALID);
}

TEST(HintApi, AFailingProgramFallsBackUnlessTheModeIsStrict) {
  Fixture f({gfnttest::build_glyf_glyph({kSquare}, op({0x60}))}, {{300, 40}});   // ADD, no operands
  GFNT_Result r;

  EXPECT_EQ(rows(f, GFNT_HINT_MODE_FONT, &r), 4u);
  EXPECT_EQ(r, GFNT_OK);
  rows(f, GFNT_HINT_MODE_FONT_STRICT, &r);
  EXPECT_EQ(r, GFNT_ERR_CORRUPT);
}

TEST(HintApi, AGlyphWithNoProgramIsNotAFailureInStrictMode) {
  Fixture f({gfnttest::build_glyf_glyph({kSquare})}, {{300, 40}});
  GFNT_Result r;

  EXPECT_EQ(rows(f, GFNT_HINT_MODE_FONT_STRICT, &r), 4u);
  EXPECT_EQ(r, GFNT_OK);
}

TEST(HintApi, GaspSaysWhichSizesAreFitted) {
  // Below 9 pixels per em: smoothing only. From there up: grid-fit and smooth.
  Fixture f = square(hinting_program(), gasp({{8, 0x0002}, {0xFFFF, 0x0003}}));

  EXPECT_FALSE(gfnt_face_gasp_gridfit(f.face, 8));
  EXPECT_TRUE(gfnt_face_gasp_gridfit(f.face, 9));
  EXPECT_TRUE(gfnt_face_gasp_gridfit(f.face, 500));
  EXPECT_EQ(rows(f, GFNT_HINT_MODE_FONT), 3u);        // 10 pixels per em: fitted
  Fixture none = square(hinting_program(), gasp({{0xFFFF, 0x0002}}));
  EXPECT_EQ(rows(none, GFNT_HINT_MODE_FONT), 4u);     // the font asks for none
}

TEST(HintApi, AFaceWithNoGaspOrATruncatedOneWantsHinting) {
  Fixture plain = square(hinting_program());
  Fixture cut = square(hinting_program(), Bytes{0, 1, 0, 5, 0, 8});

  EXPECT_TRUE(gfnt_face_gasp_gridfit(plain.face, 12));
  EXPECT_TRUE(gfnt_face_gasp_gridfit(cut.face, 12));
  EXPECT_TRUE(gfnt_face_gasp_gridfit(nullptr, 12));
}

TEST(HintApi, AContextRefusesWhatCannotBeHintedFor) {
  Fixture f({gfnttest::build_glyf_glyph({kSquare})}, {{300, 40}});
  GFNT_HintContext * context = nullptr;
  GFNT_Error error{};

  EXPECT_EQ(gfnt_hint_context_create(f.face, 0, GFNT_HINT_VERSION_AUTO, &context, &error),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_hint_context_create(nullptr, 10, GFNT_HINT_VERSION_AUTO, &context, &error),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_hint_context_create(f.face, 10, static_cast<GFNT_HintVersion>(7), &context,
      &error), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_hint_context_create(f.face, 1000000, GFNT_HINT_VERSION_AUTO, &context, &error),
      GFNT_ERR_LIMIT);
  EXPECT_EQ(context, nullptr);
  gfnt_hint_context_destroy(nullptr);
}

TEST(HintApi, AHintedOutlineSaysWhatBecameOfTheGlyph) {
  Fixture f({gfnttest::build_glyf_glyph({kSquare}, hinting_program()),
             gfnttest::build_glyf_glyph({kSquare}),
             gfnttest::build_glyf_glyph({kSquare}, op({0x60}))},
      {{300, 40}, {300, 40}, {300, 40}});
  GFNT_HintContext * context = nullptr;
  GFNT_Error error{};
  const GFNT_HintStatus expected[3] = {GFNT_HINT_STATUS_HINTED,
      GFNT_HINT_STATUS_UNHINTED_NO_PROGRAM, GFNT_HINT_STATUS_UNHINTED_PROGRAM_FAILED};

  ASSERT_EQ(gfnt_hint_context_create(f.face, 10, GFNT_HINT_VERSION_35, &context, &error),
      GFNT_OK);
  for (uint32_t g = 0; g < 3; ++g) {
    GFNT_Outline * outline = nullptr;
    GFNT_HintStatus status = GFNT_HINT_STATUS_HINTED;
    GFNT_F26Dot6 advance = 0;
    GFNT_Point p{};

    ASSERT_EQ(gfnt_face_hinted_outline(f.face, context, g, nullptr, &outline, &status,
        &advance, &error), GFNT_OK) << g;
    EXPECT_EQ(status, expected[g]) << g;
    EXPECT_EQ(advance, 320) << g;
    ASSERT_EQ(gfnt_outline_point_count(outline), 4u);
    gfnt_outline_point_at(outline, 2, &p, nullptr);
    EXPECT_EQ(p.y, g == 0 ? 192 : 213) << g;
    gfnt_outline_destroy(outline);
  }
  EXPECT_EQ(gfnt_face_hinted_outline(f.face, nullptr, 0, nullptr, nullptr, nullptr, nullptr,
      &error), GFNT_ERR_INVALID);
  gfnt_hint_context_destroy(context);
}

TEST(HintApi, AFaceWithoutTrueTypeOutlinesRendersAsItAlwaysDid) {
  const std::string path = gfnttest::data("fonts/cff2-fds.otf");
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_HintContext * context = nullptr;
  GFNT_Error error{};
  GFNT_RasterOptions hinted{};
  GFNT_RasterOptions none{};
  GFNT_Coverage a{};
  GFNT_Coverage b{};

  ASSERT_EQ(gfnt_blob_create_file(path.c_str(), nullptr, nullptr, &blob, &error), GFNT_OK);
  ASSERT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error), GFNT_OK);
  EXPECT_EQ(gfnt_hint_context_create(face, 16, GFNT_HINT_VERSION_AUTO, &context, &error),
      GFNT_ERR_UNSUPPORTED);
  none.hint_mode = GFNT_HINT_MODE_NONE;
  ASSERT_EQ(gfnt_face_render_glyph(face, 1, 16, &hinted, nullptr, &a, &error), GFNT_OK);
  ASSERT_EQ(gfnt_face_render_glyph(face, 1, 16, &none, nullptr, &b, &error), GFNT_OK);
  EXPECT_EQ(a.width, b.width);
  EXPECT_EQ(a.height, b.height);
  EXPECT_EQ(a.stride * a.height, b.stride * b.height);
  EXPECT_EQ(0, a.data && b.data ? memcmp(a.data, b.data, a.stride * a.height) : 0);
  gfnt_coverage_destroy(&a);
  gfnt_coverage_destroy(&b);
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
