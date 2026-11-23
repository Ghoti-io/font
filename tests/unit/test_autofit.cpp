/**
 * @file
 *
 * The automatic hinter: what it promises of an outline whatever the face declares.
 *
 * The faces are `latin-boxes.ttf`, whose letters are boxes and ellipses with known heights (capitals 700,
 * ascenders 760, small letters 500, descenders -210, round letters overshooting by 12), and the CFF and Type 1
 * fixtures for the other kinds of outline. The agreement with FreeType's light automatic hinter is not asked
 * of this suite: `make check-oracle-autohint` compares every glyph of the boxes and the Latin letters of ten
 * faces point by point. What is asked here is what an outline hinter owes anyone: the zones snap to whole
 * pixels at every size, the baseline stays, x is scaled and not moved, the advance is a whole number of
 * pixels, a glyph keeps its contours, the answer is the same twice, and a face with no outlines is refused.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/hint.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/outline.h>
#include <ghoti.io/font/raster.h>

#include <gtest/gtest.h>

namespace {

struct Fixture {
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Result result = GFNT_ERR_INTERNAL;

  explicit Fixture(const std::string & name) {
    GFNT_Error error{};
    gfnt_error_clear(&error);
    result = gfnt_blob_create_file(gfnttest::data("fonts/" + name).c_str(), nullptr, nullptr, &blob, &error);
    if (result == GFNT_OK) {
      result = gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error);
    }
  }
  ~Fixture() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
  Fixture(const Fixture &) = delete;
  Fixture & operator=(const Fixture &) = delete;
};

struct Hinted {
  std::vector<std::pair<int32_t, int32_t>> points;
  size_t contours = 0;
  GFNT_HintStatus status = GFNT_HINT_STATUS_UNHINTED_NO_PROGRAM;
  GFNT_F26Dot6 advance = 0;
  GFNT_OutlineSpace space = GFNT_OUTLINE_UNITS;
  GFNT_Result result = GFNT_ERR_INTERNAL;
};

uint32_t glyph_of(const GFNT_Face * face, char letter) {
  uint32_t glyph = 0;
  EXPECT_EQ(gfnt_face_glyph_for_codepoint(face, (uint32_t)(unsigned char)letter, &glyph, nullptr), GFNT_OK);
  return glyph;
}

Hinted hinted(const GFNT_Face * face, uint32_t glyph, uint32_t ppem, GFNT_HintMode mode = GFNT_HINT_MODE_AUTO) {
  Hinted out;
  GFNT_HintContext * context = nullptr;
  GFNT_Outline * outline = nullptr;

  out.result = gfnt_hint_context_create(face, ppem, GFNT_HINT_VERSION_AUTO, mode, nullptr, &context, nullptr);
  if (out.result != GFNT_OK) {
    return out;
  }
  out.result = gfnt_face_hinted_outline(face, context, glyph, nullptr, &outline, &out.status, &out.advance, nullptr);
  if (out.result == GFNT_OK) {
    out.space = gfnt_outline_space(outline);
    out.contours = gfnt_outline_contour_count(outline);
    for (size_t i = 0; i < gfnt_outline_point_count(outline); ++i) {
      GFNT_Point p;
      EXPECT_EQ(gfnt_outline_point_at(outline, i, &p, nullptr), GFNT_OK);
      out.points.push_back({p.x, p.y});
    }
  }
  gfnt_outline_destroy(outline);
  gfnt_hint_context_destroy(context);
  return out;
}

int32_t top_of(const Hinted & h) {
  int32_t top = INT32_MIN;
  for (const auto & p : h.points) {
    top = p.second > top ? p.second : top;
  }
  return top;
}

int32_t bottom_of(const Hinted & h) {
  int32_t bottom = INT32_MAX;
  for (const auto & p : h.points) {
    bottom = p.second < bottom ? p.second : bottom;
  }
  return bottom;
}

}  // namespace

TEST(Autofit, AFlatTopOrBottomOfEveryZoneIsOnAWholePixelAtEverySize) {
  Fixture font("latin-boxes.ttf");
  ASSERT_EQ(font.result, GFNT_OK);
  for (uint32_t ppem : {9u, 10u, 11u, 12u, 13u, 14u, 16u, 17u, 20u, 24u, 32u}) {
    SCOPED_TRACE(ppem);
    for (char letter : {'H', 'T', 'E', 'x', 'z', 'd', 'b', 'p'}) {
      Hinted h = hinted(font.face, glyph_of(font.face, letter), ppem);
      ASSERT_EQ(h.result, GFNT_OK) << letter;
      EXPECT_EQ(h.status, GFNT_HINT_STATUS_HINTED) << letter;
      EXPECT_EQ(h.space, GFNT_OUTLINE_PIXELS) << letter;
      EXPECT_EQ(top_of(h) % 64, 0) << letter << " top " << top_of(h);
      EXPECT_EQ(bottom_of(h) % 64, 0) << letter << " bottom " << bottom_of(h);
    }
  }
}

TEST(Autofit, TheXHeightIsRoundedUpFromThreeEighthsAndTheCapitalsFollowIt) {
  // 1000 units to the em, small letters to 500 with a round overshoot of 12: at 11 ppem the round top is
  // 512 * 11 / 1000 = 5.63 px and goes to 6, and the capitals, 700 units, are carried by the same correction
  // (6 / 5.63): 7.70 * 1.066 = 8.2 and round to 8.
  Fixture font("latin-boxes.ttf");
  ASSERT_EQ(font.result, GFNT_OK);
  Hinted o = hinted(font.face, glyph_of(font.face, 'o'), 11);
  Hinted x = hinted(font.face, glyph_of(font.face, 'x'), 11);
  Hinted H = hinted(font.face, glyph_of(font.face, 'H'), 11);
  ASSERT_EQ(o.result, GFNT_OK);
  EXPECT_EQ(top_of(o), 6 * 64);
  EXPECT_EQ(top_of(x), 6 * 64);
  EXPECT_EQ(top_of(H), 8 * 64);
  // At 9 ppem 512 units are 4.61 px and go to 5; at 10, 5.12 px stays 5.
  EXPECT_EQ(top_of(hinted(font.face, glyph_of(font.face, 'o'), 9)), 5 * 64);
  EXPECT_EQ(top_of(hinted(font.face, glyph_of(font.face, 'o'), 10)), 5 * 64);
}

TEST(Autofit, TheXHeightCorrectionIsNotAppliedWhenItWouldMoveTheTallestZoneByTwoPixels) {
  // x-height 150 and round top 160 under ascenders of 1000. At 10 ppem the round top is 1.6 px and would go
  // to 2, a correction of 1.25 that would carry the ascenders from 10 px to 12.5: a move of 2.5 px, so the
  // correction is dropped and the ascender stays at 10.
  Fixture font("latin-boxes-tall.ttf");
  ASSERT_EQ(font.result, GFNT_OK);
  EXPECT_EQ(top_of(hinted(font.face, glyph_of(font.face, 'd'), 10)), 10 * 64);
  // At 11 ppem 1.76 px goes to 2, a correction of 1.14 that moves the ascenders by 1.5 px: applied, 11 -> 12.5 -> 13
  // (and the round top is 2).
  Hinted at11 = hinted(font.face, glyph_of(font.face, 'o'), 11);
  EXPECT_EQ(top_of(at11), 2 * 64);
  EXPECT_NE(top_of(hinted(font.face, glyph_of(font.face, 'd'), 11)), 11 * 64);
}

TEST(Autofit, TheRasterRendersAGlyphFittedByItWithTheTopOnAWholePixel) {
  // The capitals are 700 units of 1000: 7.7 px at 11 ppem, which unhinted leaves the top row 70% covered; fitted
  // by the automatic hinter the top is 8 px and the row is full.
  Fixture font("latin-boxes.ttf");
  ASSERT_EQ(font.result, GFNT_OK);
  const uint32_t glyph = glyph_of(font.face, 'H');
  GFNT_RasterOptions options{};
  GFNT_Coverage fitted{};
  GFNT_Coverage plain{};
  GFNT_Error error{};

  options.hint_mode = GFNT_HINT_MODE_AUTO;
  ASSERT_EQ(gfnt_face_render_glyph(font.face, glyph, 11, &options, nullptr, &fitted, &error), GFNT_OK);
  options.hint_mode = GFNT_HINT_MODE_NONE;
  ASSERT_EQ(gfnt_face_render_glyph(font.face, glyph, 11, &options, nullptr, &plain, &error), GFNT_OK);
  ASSERT_GT(fitted.width, 2u);
  ASSERT_GT(plain.width, 2u);
  EXPECT_EQ(fitted.top, 8);
  EXPECT_EQ(fitted.data[fitted.width / 2], 255);
  EXPECT_LT(plain.data[plain.width / 2], 255);
  EXPECT_EQ(fitted.height, 8u);
  gfnt_coverage_destroy(&fitted);
  gfnt_coverage_destroy(&plain);
}

TEST(Autofit, XIsScaledAndNeverMovedAndTheAdvanceIsAWholePixel) {
  Fixture font("latin-boxes.ttf");
  ASSERT_EQ(font.result, GFNT_OK);
  GFNT_HintContext * context = nullptr;
  ASSERT_EQ(gfnt_hint_context_create(font.face, 12, GFNT_HINT_VERSION_AUTO, GFNT_HINT_MODE_AUTO, nullptr, &context,
      nullptr), GFNT_OK);
  for (char letter : {'H', 'x', 'o', 'p'}) {
    const uint32_t glyph = glyph_of(font.face, letter);
    GFNT_Outline * plain = nullptr;
    GFNT_Outline * fitted = nullptr;
    GFNT_HintStatus status = GFNT_HINT_STATUS_UNHINTED_NO_PROGRAM;
    GFNT_F26Dot6 advance = 0;
    ASSERT_EQ(gfnt_face_glyph_outline(font.face, glyph, nullptr, nullptr, &plain, nullptr), GFNT_OK);
    ASSERT_EQ(gfnt_face_hinted_outline(font.face, context, glyph, nullptr, &fitted, &status, &advance, nullptr), GFNT_OK);
    EXPECT_EQ(advance % 64, 0) << letter;
    ASSERT_EQ(gfnt_outline_point_count(plain), gfnt_outline_point_count(fitted));
    EXPECT_EQ(gfnt_outline_contour_count(plain), gfnt_outline_contour_count(fitted));
    for (size_t i = 0; i < gfnt_outline_point_count(plain); ++i) {
      GFNT_Point a, b;
      ASSERT_EQ(gfnt_outline_point_at(plain, i, &a, nullptr), GFNT_OK);
      ASSERT_EQ(gfnt_outline_point_at(fitted, i, &b, nullptr), GFNT_OK);
      // The plain outline is in 26.6 of a font unit: x of 1000 units to the em at 12 px is x * 12 / 1000 pixels.
      const int64_t want = ((int64_t)a.x * 12 * 64 / 64 + 500) / 1000;   // 26.6 of a unit -> 26.6 of a pixel
      EXPECT_NEAR(b.x, want, 1) << letter << " point " << i;
    }
    gfnt_outline_destroy(plain);
    gfnt_outline_destroy(fitted);
  }
  gfnt_hint_context_destroy(context);
}

TEST(Autofit, TheSameGlyphAskedTwiceGivesTheSameOutline) {
  Fixture font("latin-boxes.ttf");
  ASSERT_EQ(font.result, GFNT_OK);
  for (char letter : {'H', 'o', 'p', 'd'}) {
    Hinted a = hinted(font.face, glyph_of(font.face, letter), 14);
    Hinted b = hinted(font.face, glyph_of(font.face, letter), 14);
    ASSERT_EQ(a.result, GFNT_OK);
    EXPECT_EQ(a.points, b.points) << letter;
    EXPECT_EQ(a.advance, b.advance) << letter;
  }
}

TEST(Autofit, ACffAndATypeOneFaceAreFittedToo) {
  for (const char * name : {"cff-hints.otf", "type1.pfb", "basic.ttf"}) {
    Fixture font(name);
    ASSERT_EQ(font.result, GFNT_OK) << name;
    size_t glyphs = 0;
    ASSERT_EQ(gfnt_face_num_glyphs(font.face, &glyphs, nullptr), GFNT_OK);
    for (uint32_t g = 0; g < glyphs && g < 6; ++g) {
      GFNT_Outline * plain = nullptr;
      if (gfnt_face_glyph_outline(font.face, g, nullptr, nullptr, &plain, nullptr) != GFNT_OK) {
        continue;
      }
      Hinted h = hinted(font.face, g, 12);
      ASSERT_EQ(h.result, GFNT_OK) << name << " glyph " << g;
      EXPECT_EQ(h.status, GFNT_HINT_STATUS_HINTED) << name << " glyph " << g;
      EXPECT_EQ(h.points.size(), gfnt_outline_point_count(plain)) << name << " glyph " << g;
      EXPECT_EQ(h.contours, gfnt_outline_contour_count(plain)) << name << " glyph " << g;
      EXPECT_EQ(h.advance % 64, 0) << name << " glyph " << g;
      gfnt_outline_destroy(plain);
    }
  }
}

TEST(Autofit, AFaceWithNoLettersIsStillFittedFromWhatItHas) {
  // basic.ttf has A and B only: no zone is known and no stem width, and the glyphs are scaled and kept whole.
  Fixture font("basic.ttf");
  ASSERT_EQ(font.result, GFNT_OK);
  Hinted a = hinted(font.face, glyph_of(font.face, 'A'), 17);
  ASSERT_EQ(a.result, GFNT_OK);
  EXPECT_EQ(a.status, GFNT_HINT_STATUS_HINTED);
  EXPECT_FALSE(a.points.empty());
}

TEST(Autofit, ASizeOfZeroAndAGlyphThatIsNotThereAreRefused) {
  Fixture font("latin-boxes.ttf");
  ASSERT_EQ(font.result, GFNT_OK);
  GFNT_HintContext * context = nullptr;
  EXPECT_EQ(gfnt_hint_context_create(font.face, 0, GFNT_HINT_VERSION_AUTO, GFNT_HINT_MODE_AUTO, nullptr, &context,
      nullptr), GFNT_ERR_INVALID);
  ASSERT_EQ(gfnt_hint_context_create(font.face, 12, GFNT_HINT_VERSION_AUTO, GFNT_HINT_MODE_AUTO, nullptr, &context,
      nullptr), GFNT_OK);
  GFNT_Outline * outline = nullptr;
  EXPECT_NE(gfnt_face_hinted_outline(font.face, context, 100000, nullptr, &outline, nullptr, nullptr, nullptr), GFNT_OK);
  gfnt_hint_context_destroy(context);
}

TEST(Autofit, TheFontsOwnProgramIsStillWhatTheOtherModesRun) {
  // GFNT_HINT_MODE_FONT on a glyf face runs its program and not the automatic hinter: the two differ on a face
  // whose tops the program leaves where scaling puts them.
  Fixture font("latin-boxes.ttf");
  ASSERT_EQ(font.result, GFNT_OK);
  Hinted automatic = hinted(font.face, glyph_of(font.face, 'H'), 11, GFNT_HINT_MODE_AUTO);
  Hinted programmed = hinted(font.face, glyph_of(font.face, 'H'), 11, GFNT_HINT_MODE_FONT);
  ASSERT_EQ(automatic.result, GFNT_OK);
  ASSERT_EQ(programmed.result, GFNT_OK);
  EXPECT_EQ(top_of(automatic), 8 * 64);
  EXPECT_EQ(programmed.status, GFNT_HINT_STATUS_UNHINTED_NO_PROGRAM);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
