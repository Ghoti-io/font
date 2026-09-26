/**
 * @file
 *
 * `glyf` and `loca`, read from fonts **fontTools** wrote.
 *
 * Every number in this file came out of the reference, not out of this library:
 * the paths and boxes are what `fontTools`' own pen and its own
 * `Glyph.getCoordinates()` produce for `tests/data/fonts/outline-*.ttf`, read
 * off once and written down here. That is the point of the split from
 * `test_outline.cpp`, where the rules are asserted against the specification
 * from hand-built outlines: these fonts are the only inputs in the unit suite
 * that this library did not write, so they are the only ones that can disagree
 * with it.
 *
 * `tools/oracle/glyf_diff.py` does the same comparison over 327 real fonts and
 * needs a container; this needs only the repository, which is what makes the
 * outline reader covered by `make test` on a machine with no container engine.
 *
 * documentation/design.md sections 7.3, 14.5 and 14.7.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <map>
#include <string>
#include <vector>

#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/outline.h>

namespace {

/** A fixture read from disk, with a blob and a face over it. */
struct Font {
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Error error{};

  explicit Font(const std::string & name) {
    const std::string path = gfnttest::data("fonts/" + name);
    // EXPECT rather than ASSERT: ASSERT_* returns, and a constructor cannot.
    // A missing fixture is a failure of this suite rather than of the loader,
    // so it says which file instead of leaving a null face to fail later.
    EXPECT_EQ(gfnt_blob_create_file(path.c_str(), nullptr, nullptr, &blob,
        &error), GFNT_OK) << "could not read " << path;
    EXPECT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error), GFNT_OK)
        << name << ": " << (error.message ? error.message : "");
  }
  ~Font() {
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
  Font(const Font &) = delete;
  Font & operator=(const Font &) = delete;

  operator const GFNT_Face *() const { return face; }
};

/** An outline loaded for one glyph, destroyed with the object. */
struct Loaded {
  GFNT_Outline * outline = nullptr;
  GFNT_Error error{};
  GFNT_Result result = GFNT_ERR_INTERNAL;

  Loaded(const GFNT_Face * face, uint32_t glyph) {
    gfnt_error_clear(&error);
    result = gfnt_face_glyph_outline(face, glyph, nullptr, nullptr, &outline,
        &error);
  }
  ~Loaded() { gfnt_outline_destroy(outline); }
  Loaded(const Loaded &) = delete;
  Loaded & operator=(const Loaded &) = delete;
};

/** The path as one string per segment, in whole font units. */
std::vector<std::string> path_of(const GFNT_Outline * outline) {
  std::vector<std::string> lines;
  GFNT_OutlineSink sink{};
  sink.move_to = [](void * user, GFNT_Point to) -> GFNT_Result {
    static_cast<std::vector<std::string> *>(user)->push_back(
        "move " + std::to_string(to.x / 64) + " " + std::to_string(to.y / 64));
    return GFNT_OK;
  };
  sink.line_to = [](void * user, GFNT_Point to) -> GFNT_Result {
    static_cast<std::vector<std::string> *>(user)->push_back(
        "line " + std::to_string(to.x / 64) + " " + std::to_string(to.y / 64));
    return GFNT_OK;
  };
  sink.quad_to = [](void * user, GFNT_Point c, GFNT_Point to) -> GFNT_Result {
    static_cast<std::vector<std::string> *>(user)->push_back(
        "quad " + std::to_string(c.x / 64) + " " + std::to_string(c.y / 64)
        + " " + std::to_string(to.x / 64) + " " + std::to_string(to.y / 64));
    return GFNT_OK;
  };
  sink.close = [](void * user) -> GFNT_Result {
    static_cast<std::vector<std::string> *>(user)->push_back("close");
    return GFNT_OK;
  };
  EXPECT_EQ(gfnt_outline_decompose(outline, &sink, &lines, nullptr), GFNT_OK);
  return lines;
}

/** A box in whole font units, or "empty". */
std::string box_string(const GFNT_Box & box) {
  if (gfnt_box_is_empty(&box)) {
    return "empty";
  }
  return std::to_string(box.x_min / 64) + " " + std::to_string(box.y_min / 64)
      + " " + std::to_string(box.x_max / 64) + " "
      + std::to_string(box.y_max / 64);
}

/** The glyph order of outline-simple.ttf and outline-composite.ttf. */
enum Glyph {
  kNotdef = 0,
  kSpace = 1,
  kQuadExplicit = 2,
  kQuadImplied = 3,
  kOffCurveStart = 4,
  kOffCurveStartRotated = 5,
  kAllOffCurve = 6,
  kTwoContours = 7,
  kSinglePoint = 8,
  kWithInstructions = 9,
  kManyPoints = 10,
  kArch = 11,
  kCompOffset = 12,
  kCompWordOffset = 13,
  kCompScale = 14,
  kCompScaledOffset = 15,
  kCompUnscaledOffset = 16,
  kCompXyScale = 17,
  kCompTwoByTwo = 18,
  kCompPointMatch = 19,
  kCompPointMatchScaled = 20,
  kCompNested = 21,
  kCompUseMyMetrics = 22,
  kCompRoundAndOverlap = 23,
  kCompPointMatchHigh = 24,
  kCompInstructions = 25,
};

TEST(Glyf, TheSimpleFixtureHasTheGlyphsItsManifestSays) {
  Font font("outline-simple.ttf");
  size_t glyphs = 0;

  ASSERT_EQ(gfnt_face_num_glyphs(font, &glyphs, nullptr), GFNT_OK);
  EXPECT_EQ(glyphs, 12u);
  EXPECT_TRUE(gfnt_face_has_outlines(font));
}

TEST(Glyf, AnExplicitQuadraticIsReadAsTheReferenceDrawsIt) {
  Font font("outline-simple.ttf");
  Loaded loaded(font, kQuadExplicit);

  ASSERT_EQ(loaded.result, GFNT_OK) << loaded.error.message;
  EXPECT_EQ(gfnt_outline_point_count(loaded.outline), 4u);
  EXPECT_EQ(gfnt_outline_contour_count(loaded.outline), 1u);
  EXPECT_EQ(path_of(loaded.outline), (std::vector<std::string>{
      "move 100 0",
      "quad 100 400 400 400",
      "quad 400 0 100 0",
      "close",
  }));
}

TEST(Glyf, AnImpliedMidpointIsSynthesisedWhereTheFontLeftItOut) {
  Font font("outline-simple.ttf");
  Loaded loaded(font, kQuadImplied);

  ASSERT_EQ(loaded.result, GFNT_OK) << loaded.error.message;
  // Four points in the file, and a fifth on the path that is in no font.
  EXPECT_EQ(gfnt_outline_point_count(loaded.outline), 4u);
  EXPECT_EQ(path_of(loaded.outline), (std::vector<std::string>{
      "move 100 0",
      "quad 200 500 300 500",
      "quad 400 500 500 0",
      "close",
  }));
}

TEST(Glyf, AContourStoredBeginningOffCurveIsReadAsTheReferenceDrawsIt) {
  Font font("outline-simple.ttf");
  Loaded loaded(font, kOffCurveStart);

  ASSERT_EQ(loaded.result, GFNT_OK) << loaded.error.message;
  EXPECT_EQ(path_of(loaded.outline), (std::vector<std::string>{
      "move 100 0",
      "quad 100 400 250 400",
      "quad 400 400 400 200",
      "quad 400 0 100 0",
      "close",
  }));
}

TEST(Glyf, AnAllOffCurveContourIsReadAsTheReferenceDrawsIt) {
  Font font("outline-simple.ttf");
  Loaded loaded(font, kAllOffCurve);

  ASSERT_EQ(loaded.result, GFNT_OK) << loaded.error.message;
  EXPECT_EQ(path_of(loaded.outline), (std::vector<std::string>{
      "move 250 0",
      "quad 100 0 100 200",
      "quad 100 400 250 400",
      "quad 400 400 400 200",
      "quad 400 0 250 0",
      "close",
  }));
}

TEST(Glyf, TheStatedBoxIsTheCoordinateBoxAndNotTheCurvesOwn) {
  // `glyf`'s xMin/yMax are the **coordinate** box: the specification says
  // "minimum x for coordinate data", and fontTools writes exactly that -
  // `Glyph.recalcBounds()` takes the bounds of the points. So the stated box is
  // what gfnt_outline_control_box() computes, and comparing it against
  // gfnt_outline_bounds() is a category error: the tight box of a curve whose
  // control point lies outside it is *smaller*.
  //
  // This test compared the tight box until `arch` existed, and passed - because
  // in every other fixture the two coincide. A control point inside the curve's
  // own extent is the common case and the case that cannot tell them apart.
  for (const char * name : {"outline-simple.ttf", "outline-composite.ttf",
      "outline-loca-long.ttf"}) {
    Font font(name);
    size_t glyphs = 0;
    ASSERT_EQ(gfnt_face_num_glyphs(font, &glyphs, nullptr), GFNT_OK);
    for (uint32_t glyph = 0; glyph < glyphs; ++glyph) {
      Loaded loaded(font, glyph);
      ASSERT_EQ(loaded.result, GFNT_OK) << name << " glyph " << glyph << ": "
                                        << loaded.error.message;
      GFNT_Box stated{};
      GFNT_Box control{};
      GFNT_Box drawn{};
      ASSERT_EQ(gfnt_face_glyph_stated_box(font, glyph, &stated, nullptr),
          GFNT_OK);
      ASSERT_EQ(gfnt_outline_control_box(loaded.outline, &control), GFNT_OK);
      ASSERT_EQ(gfnt_outline_bounds(loaded.outline, &drawn), GFNT_OK);
      EXPECT_EQ(box_string(stated), box_string(control))
          << name << " glyph " << glyph;
      // And the curve is inside its control points, always.
      if (!gfnt_box_is_empty(&drawn)) {
        EXPECT_LE(control.x_min, drawn.x_min) << name << " glyph " << glyph;
        EXPECT_LE(control.y_min, drawn.y_min) << name << " glyph " << glyph;
        EXPECT_GE(control.x_max, drawn.x_max) << name << " glyph " << glyph;
        EXPECT_GE(control.y_max, drawn.y_max) << name << " glyph " << glyph;
      }
    }
  }
}

TEST(Glyf, AnArchsCurveIsStrictlyInsideItsControlPoints) {
  // The glyph that makes the test above mean something: without one where the
  // two boxes differ, "the stated box is the control box" and "the stated box
  // is the curve" are the same assertion.
  Font font("outline-simple.ttf");
  Loaded loaded(font, kArch);
  GFNT_Box stated{};
  GFNT_Box control{};
  GFNT_Box drawn{};

  ASSERT_EQ(loaded.result, GFNT_OK) << loaded.error.message;
  ASSERT_EQ(gfnt_face_glyph_stated_box(font, kArch, &stated, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_outline_control_box(loaded.outline, &control), GFNT_OK);
  ASSERT_EQ(gfnt_outline_bounds(loaded.outline, &drawn), GFNT_OK);
  EXPECT_EQ(box_string(stated), "0 0 100 100");
  EXPECT_EQ(box_string(control), "0 0 100 100");
  // The arch peaks halfway to its control point.
  EXPECT_EQ(box_string(drawn), "0 0 100 50");
}

TEST(Glyf, AGlyphWithNoDescriptionIsAnEmptyOutlineAndNotAFailure) {
  Font font("outline-simple.ttf");
  Loaded loaded(font, kSpace);

  ASSERT_EQ(loaded.result, GFNT_OK) << loaded.error.message;
  EXPECT_EQ(gfnt_outline_point_count(loaded.outline), 0u);
  EXPECT_EQ(gfnt_outline_contour_count(loaded.outline), 0u);
  EXPECT_EQ(path_of(loaded.outline), std::vector<std::string>{});
}

TEST(Glyf, AGlyphsInstructionsAreSkippedByLengthAndNeverRun) {
  Font font("outline-simple.ttf");
  Loaded plain(font, kQuadExplicit);
  Loaded hinted(font, kWithInstructions);

  ASSERT_EQ(hinted.result, GFNT_OK) << hinted.error.message;
  // Two bytes of instructions sit between the flag stream's start and where a
  // reader that forgot to skip them would land, so the coordinates are the
  // test: the same shape means the skip happened.
  EXPECT_EQ(path_of(hinted.outline), path_of(plain.outline));
}

TEST(Glyf, AOnePointContourLoadsAndDrawsNothing) {
  Font font("outline-simple.ttf");
  Loaded loaded(font, kSinglePoint);

  ASSERT_EQ(loaded.result, GFNT_OK) << loaded.error.message;
  EXPECT_EQ(gfnt_outline_point_count(loaded.outline), 1u);
  EXPECT_EQ(path_of(loaded.outline), (std::vector<std::string>{
      "move 300 300", "close"}));
}

TEST(Glyf, TwoContoursKeepTheirSeparateRanges) {
  Font font("outline-simple.ttf");
  Loaded loaded(font, kTwoContours);
  size_t first = 0;
  size_t count = 0;

  ASSERT_EQ(loaded.result, GFNT_OK) << loaded.error.message;
  EXPECT_EQ(gfnt_outline_contour_count(loaded.outline), 2u);
  EXPECT_EQ(gfnt_outline_point_count(loaded.outline), 8u);
  ASSERT_EQ(gfnt_outline_contour_at(loaded.outline, 0, &first, &count),
      GFNT_OK);
  EXPECT_EQ(first, 0u);
  EXPECT_EQ(count, 4u);
  ASSERT_EQ(gfnt_outline_contour_at(loaded.outline, 1, &first, &count),
      GFNT_OK);
  EXPECT_EQ(first, 4u);
  EXPECT_EQ(count, 4u);
}

TEST(Glyf, TheSameContourStoredTwoWaysDrawsTheSameBox) {
  // quad-implied and off-curve-start-rotated are one contour written from two
  // different points of its cycle. Where the path starts is a choice; the
  // region is not, and the box has no starting point.
  Font font("outline-simple.ttf");
  Loaded plain(font, kQuadImplied);
  Loaded rotated(font, kOffCurveStartRotated);
  GFNT_Box first{};
  GFNT_Box second{};

  ASSERT_EQ(plain.result, GFNT_OK);
  ASSERT_EQ(rotated.result, GFNT_OK);
  ASSERT_EQ(gfnt_outline_bounds(plain.outline, &first), GFNT_OK);
  ASSERT_EQ(gfnt_outline_bounds(rotated.outline, &second), GFNT_OK);
  EXPECT_EQ(box_string(first), box_string(second));
  EXPECT_EQ(box_string(first), "100 0 500 500");
}

// --- composites ------------------------------------------------------------

/** Which glyphs are composites, per the font itself. */
TEST(Glyf, TheFontSaysWhichGlyphsItAssembled) {
  Font font("outline-composite.ttf");
  size_t glyphs = 0;

  ASSERT_EQ(gfnt_face_num_glyphs(font, &glyphs, nullptr), GFNT_OK);
  EXPECT_EQ(glyphs, 26u);
  for (uint32_t glyph = 0; glyph < glyphs; ++glyph) {
    bool composite = false;
    ASSERT_EQ(gfnt_face_glyph_is_composite(font, glyph, &composite, nullptr),
        GFNT_OK) << glyph;
    EXPECT_EQ(composite, glyph >= kCompOffset) << "glyph " << glyph;
  }
}

TEST(Glyf, AByteOffsetPlacesItsComponent) {
  Font font("outline-composite.ttf");
  Loaded loaded(font, kCompOffset);

  ASSERT_EQ(loaded.result, GFNT_OK) << loaded.error.message;
  EXPECT_EQ(path_of(loaded.outline), (std::vector<std::string>{
      "move 200 50",
      "quad 200 450 500 450",
      "quad 500 50 200 50",
      "close",
  }));
}

TEST(Glyf, AWordOffsetIsReadAtItsOwnWidth) {
  Font font("outline-composite.ttf");
  Loaded loaded(font, kCompWordOffset);

  // -200 does not fit a signed byte, so this component's arguments are words.
  // A reader that reads bytes here lands 200 units away and reports success.
  ASSERT_EQ(loaded.result, GFNT_OK) << loaded.error.message;
  EXPECT_EQ(path_of(loaded.outline), (std::vector<std::string>{
      "move 400 -200",
      "quad 400 200 700 200",
      "quad 700 -200 400 -200",
      "close",
  }));
}

TEST(Glyf, ASingleScaleIsAppliedBeforeTheOffset) {
  Font font("outline-composite.ttf");
  Loaded loaded(font, kCompScale);

  // The Microsoft reading, which is the default: scale, then move. The Apple
  // reading would put this at (100,50).
  ASSERT_EQ(loaded.result, GFNT_OK) << loaded.error.message;
  EXPECT_EQ(path_of(loaded.outline), (std::vector<std::string>{
      "move 150 100",
      "quad 150 300 300 300",
      "quad 300 100 150 100",
      "close",
  }));
}

TEST(Glyf, TheScaledOffsetFlagMovesTheComponentSomewhereElse) {
  Font font("outline-composite.ttf");
  Loaded scaled(font, kCompScaledOffset);
  Loaded unscaled(font, kCompUnscaledOffset);
  Loaded implied(font, kCompScale);

  ASSERT_EQ(scaled.result, GFNT_OK) << scaled.error.message;
  ASSERT_EQ(unscaled.result, GFNT_OK);
  ASSERT_EQ(implied.result, GFNT_OK);
  // A minimal pair: one flag bit apart, and the offset is halved with the
  // component. A reader that ignores the bit puts all three in one place.
  EXPECT_EQ(path_of(scaled.outline), (std::vector<std::string>{
      "move 100 50",
      "quad 100 250 250 250",
      "quad 250 50 100 50",
      "close",
  }));
  EXPECT_NE(path_of(scaled.outline), path_of(implied.outline));
  // And the flag that states the default out loud must agree with the default.
  EXPECT_EQ(path_of(unscaled.outline), path_of(implied.outline));
}

TEST(Glyf, TwoScalesAreADifferentEncodingAndNotJustADifferentNumber) {
  Font font("outline-composite.ttf");
  Loaded loaded(font, kCompXyScale);

  ASSERT_EQ(loaded.result, GFNT_OK) << loaded.error.message;
  EXPECT_EQ(path_of(loaded.outline), (std::vector<std::string>{
      "move 50 0",
      "quad 50 100 200 100",
      "quad 200 0 50 0",
      "close",
  }));
}

TEST(Glyf, ATwoByTwoIsAppliedInTheFilesOwnOrderAndNotTransposed) {
  Font font("outline-composite.ttf");
  Loaded loaded(font, kCompTwoByTwo);

  // The one case that can tell a transposed matrix from a correct one: the
  // other two encodings are diagonal. `scale01` and `scale10` cross over
  // between the file's column-major order and this library's row-major
  // struct, and reading them straight across shears the component the wrong
  // way while reporting success. It did, until this fixture existed.
  ASSERT_EQ(loaded.result, GFNT_OK) << loaded.error.message;
  EXPECT_EQ(path_of(loaded.outline), (std::vector<std::string>{
      "move 100 25",
      "quad 100 225 400 300",
      "quad 400 100 100 25",
      "close",
  }));
}

TEST(Glyf, PointMatchingMovesAComponentOntoAPointAlreadyPlaced) {
  Font font("outline-composite.ttf");
  Loaded loaded(font, kCompPointMatch);

  ASSERT_EQ(loaded.result, GFNT_OK) << loaded.error.message;
  EXPECT_EQ(path_of(loaded.outline), (std::vector<std::string>{
      "move 100 0",
      "quad 100 400 400 400",
      "quad 400 0 100 0",
      "close",
      "move 400 400",
      "quad 500 900 600 900",
      "quad 700 900 800 400",
      "close",
  }));
}

TEST(Glyf, PointMatchingMeasuresTheComponentAfterItsOwnTransform) {
  Font font("outline-composite.ttf");
  Loaded loaded(font, kCompPointMatchScaled);

  // The matched point is the *scaled* one, so the seam closes. Matching first
  // and scaling afterwards would leave the second contour half a component
  // away from the first.
  ASSERT_EQ(loaded.result, GFNT_OK) << loaded.error.message;
  EXPECT_EQ(path_of(loaded.outline), (std::vector<std::string>{
      "move 100 0",
      "quad 100 400 400 400",
      "quad 400 0 100 0",
      "close",
      "move 400 400",
      "quad 450 650 500 650",
      "quad 550 650 600 400",
      "close",
  }));
}

TEST(Glyf, APointIndexAboveTheSignedByteBoundaryIsUnsigned) {
  Font font("outline-composite.ttf");
  Loaded loaded(font, kCompPointMatchHigh);
  Loaded reference(font, kManyPoints);

  // Point index 150 of a 200-point component. A one-byte *offset* is signed
  // and a one-byte *point index* is not, so reading this one as signed gives
  // -106 and the glyph stops loading. Every other point match in this font is
  // below 128, where the two readings agree and cannot tell them apart.
  ASSERT_EQ(loaded.result, GFNT_OK) << loaded.error.message;
  ASSERT_EQ(reference.result, GFNT_OK);
  EXPECT_EQ(gfnt_outline_contour_count(loaded.outline), 2u);
  // Point 150 of the zigzag is (450, 0), and quad-explicit's point 0 is
  // (100, 0), so the second component moves by (350, 0).
  GFNT_Point anchor{};
  ASSERT_EQ(gfnt_outline_point_at(reference.outline, 150, &anchor, nullptr),
      GFNT_OK);
  EXPECT_EQ(anchor.x / 64, 450);
  EXPECT_EQ(anchor.y / 64, 0);
  GFNT_Point moved{};
  ASSERT_EQ(gfnt_outline_point_at(loaded.outline, 200, &moved, nullptr),
      GFNT_OK);
  EXPECT_EQ(moved.x / 64, 450);
  EXPECT_EQ(moved.y / 64, 0);
}

TEST(Glyf, ACompositeInsideACompositeIsResolvedToItsPoints) {
  Font font("outline-composite.ttf");
  Loaded loaded(font, kCompNested);

  ASSERT_EQ(loaded.result, GFNT_OK) << loaded.error.message;
  EXPECT_EQ(path_of(loaded.outline), (std::vector<std::string>{
      "move 400 50",
      "quad 400 450 700 450",
      "quad 700 50 400 50",
      "close",
      "move 0 300",
      "quad 100 800 200 800",
      "quad 300 800 400 300",
      "close",
  }));
}

TEST(Glyf, TheFlagsThisLibraryIgnoresMoveNothing) {
  Font font("outline-composite.ttf");
  Loaded metrics(font, kCompUseMyMetrics);
  Loaded rounded(font, kCompRoundAndOverlap);

  // USE_MY_METRICS changes an advance, not a coordinate; ROUND_XY_TO_GRID has
  // nothing to round to without hinting, and OVERLAP_COMPOUND is advice to a
  // rasteriser. So all three must leave the component exactly where the offset
  // put it, and these two glyphs carry the same offset with different flags.
  ASSERT_EQ(metrics.result, GFNT_OK) << metrics.error.message;
  ASSERT_EQ(rounded.result, GFNT_OK) << rounded.error.message;
  EXPECT_EQ(path_of(metrics.outline), (std::vector<std::string>{
      "move 150 0",
      "quad 150 400 450 400",
      "quad 450 0 150 0",
      "close",
  }));
  EXPECT_EQ(path_of(rounded.outline), path_of(metrics.outline));
}

TEST(Glyf, ACompositesInstructionsComeAfterItsComponentsAndAreNotRead) {
  Font font("outline-composite.ttf");
  Loaded loaded(font, kCompInstructions);

  // Two components and then four instruction bytes. A reader that keeps
  // walking components past the last one reads the instructions as a
  // component record.
  ASSERT_EQ(loaded.result, GFNT_OK) << loaded.error.message;
  EXPECT_EQ(gfnt_outline_contour_count(loaded.outline), 2u);
  EXPECT_EQ(path_of(loaded.outline), (std::vector<std::string>{
      "move 100 0",
      "quad 100 400 400 400",
      "quad 400 0 100 0",
      "close",
      "move 500 0",
      "quad 500 400 800 400",
      "quad 800 0 500 0",
      "close",
  }));
}

// --- loca ------------------------------------------------------------------

TEST(Glyf, ALongLocaIsReadAtItsOwnWidth) {
  Font font("outline-loca-long.ttf");
  const GFNT_Head * head = nullptr;
  size_t glyphs = 0;

  ASSERT_EQ(gfnt_face_head(font, &head, nullptr), GFNT_OK);
  // The fixture is only this fixture if it came out long; the generator checks
  // the same thing, and this is the half that survives a hand-edited file.
  ASSERT_EQ(head->index_to_loc_format, 1);
  ASSERT_EQ(gfnt_face_num_glyphs(font, &glyphs, nullptr), GFNT_OK);
  for (uint32_t glyph = 0; glyph < glyphs; ++glyph) {
    Loaded loaded(font, glyph);
    EXPECT_EQ(loaded.result, GFNT_OK) << "glyph " << glyph << ": "
                                      << loaded.error.message;
  }
  // A short loca stores each offset halved, so reading one format as the other
  // is not an off-by-a-factor everywhere - it is a different shape. The shape
  // is what this asserts: the same glyph as the short-loca fixture.
  Font shortloca("outline-simple.ttf");
  Loaded from_long(font, kQuadExplicit);
  Loaded from_short(shortloca, kQuadExplicit);
  ASSERT_EQ(from_long.result, GFNT_OK);
  ASSERT_EQ(from_short.result, GFNT_OK);
  EXPECT_EQ(path_of(from_long.outline), path_of(from_short.outline));
}

TEST(Glyf, ALocaEntryRunningBackwardsCondemnsOneGlyphAndNotTheFont) {
  Font font("outline-broken-loca.ttf");
  size_t glyphs = 0;
  size_t corrupt = 0;
  size_t readable = 0;

  ASSERT_EQ(gfnt_face_num_glyphs(font, &glyphs, nullptr), GFNT_OK);
  for (uint32_t glyph = 0; glyph < glyphs; ++glyph) {
    Loaded loaded(font, glyph);
    if (loaded.result == GFNT_OK) {
      readable += 1;
      continue;
    }
    EXPECT_EQ(loaded.result, GFNT_ERR_CORRUPT) << "glyph " << glyph;
    EXPECT_EQ(loaded.error.glyph, glyph);
    EXPECT_NE(loaded.error.message, nullptr);
    corrupt += 1;
  }
  // M11: exactly one glyph is condemned, and the rest of the font answers.
  EXPECT_EQ(corrupt, 1u);
  EXPECT_EQ(readable, glyphs - 1u);
}

TEST(Glyf, AGlyphPastTheGlyphCountIsACallerError) {
  Font font("outline-simple.ttf");
  size_t glyphs = 0;

  ASSERT_EQ(gfnt_face_num_glyphs(font, &glyphs, nullptr), GFNT_OK);
  Loaded loaded(font, static_cast<uint32_t>(glyphs));
  EXPECT_EQ(loaded.result, GFNT_ERR_INVALID);
  EXPECT_NE(loaded.error.message, nullptr);
}

// --- refusals --------------------------------------------------------------

TEST(Glyf, TheCubicExtensionIsRefusedByNameRatherThanMisdrawn) {
  Font font("outline-cubic.ttf");
  Loaded loaded(font, kQuadExplicit);

  // A reader that ignores flag bit 0x80 draws a cubic control point as a
  // quadratic one: a different shape, and no error at all. The font states
  // the extension in head.glyphDataFormat, which is what is met first.
  EXPECT_EQ(loaded.result, GFNT_ERR_UNSUPPORTED);
  ASSERT_NE(loaded.error.message, nullptr);
  EXPECT_NE(std::string(loaded.error.message).find("glyphDataFormat"),
      std::string::npos) << loaded.error.message;
}

TEST(Glyf, ACubicControlPointIsRefusedEvenWhenTheFontClaimsToBeOrdinary) {
  Font font("outline-cubic-flag.ttf");
  const GFNT_Head * head = nullptr;
  Loaded loaded(font, kQuadExplicit);

  // The font declares glyphDataFormat 0 and contains a cubic point anyway, so
  // the per-glyph flag is the only thing that can refuse it. Without this
  // fixture that check was unreachable: `outline-cubic.ttf` trips the
  // declaration first, and a planted defect that read the flag as quadratic
  // went undetected.
  ASSERT_EQ(gfnt_face_head(font, &head, nullptr), GFNT_OK);
  ASSERT_EQ(head->glyph_data_format, 0);
  EXPECT_EQ(loaded.result, GFNT_ERR_UNSUPPORTED);
  ASSERT_NE(loaded.error.message, nullptr);
  EXPECT_NE(std::string(loaded.error.message).find("cubic control point"),
      std::string::npos) << loaded.error.message;
  // And the glyphs around it still load: one glyph's flags condemn one glyph.
  Loaded neighbour(font, kQuadImplied);
  EXPECT_EQ(neighbour.result, GFNT_OK) << neighbour.error.message;
}

TEST(Glyf, AFaceWhoseOutlinesAreCharstringsSaysSoRatherThanSayingItHasNone) {
  Font font("cff.otf");
  Loaded loaded(font, 2);

  // The distinction design.md section 5.6 is about: this face has outlines,
  // in a table phase 2 reads. "No outlines" would be a false statement.
  EXPECT_TRUE(gfnt_face_has_outlines(font));
  EXPECT_EQ(loaded.result, GFNT_ERR_UNSUPPORTED);
  ASSERT_NE(loaded.error.message, nullptr);
  EXPECT_NE(std::string(loaded.error.message).find("charstring"),
      std::string::npos) << loaded.error.message;
}

TEST(Glyf, VariationCoordinatesAreRefusedRatherThanIgnored) {
  Font font("outline-simple.ttf");
  GFNT_F2Dot14 coords[1] = {0};
  GFNT_Variation variation{coords, 1};
  GFNT_Outline * outline = nullptr;
  GFNT_Error error{};

  EXPECT_EQ(gfnt_face_glyph_outline(font, kQuadExplicit, &variation, nullptr,
      &outline, &error), GFNT_ERR_UNSUPPORTED);
  EXPECT_NE(error.message, nullptr);
  gfnt_outline_destroy(outline);
}

TEST(Glyf, NullIsACallerErrorAndCrashesNowhere) {
  Font font("outline-simple.ttf");
  GFNT_Outline * outline = nullptr;
  GFNT_Box box{};
  bool composite = false;

  EXPECT_EQ(gfnt_face_glyph_outline(nullptr, 0, nullptr, nullptr, &outline,
      nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_glyph_outline(font, 0, nullptr, nullptr, nullptr,
      nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_glyph_is_composite(nullptr, 0, &composite, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_glyph_is_composite(font, 0, nullptr, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_glyph_stated_box(nullptr, 0, &box, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_glyph_stated_box(font, 0, nullptr, nullptr),
      GFNT_ERR_INVALID);
}

TEST(Glyf, AnAllocationFailureIsReportedAndNothingLeaks) {
  Font font("outline-composite.ttf");

  // A composite allocates an outline per component and frees it again, so the
  // sweep walks a nesting as well as a growth.
  for (size_t fail_at = 0; fail_at < 12; ++fail_at) {
    gfnttest::FailingAllocator allocator(fail_at, 0);
    GFNT_Outline * outline = nullptr;
    GFNT_Error error{};
    GFNT_Result result = gfnt_face_glyph_outline(font, kCompNested, nullptr,
        allocator.get(), &outline, &error);

    if (result != GFNT_OK) {
      EXPECT_EQ(result, GFNT_ERR_OOM) << "at request " << fail_at;
      EXPECT_EQ(outline, nullptr);
    }
    gfnt_outline_destroy(outline);
  }
}

TEST(Glyf, ThePointCapIsAPromiseAndIsEnforced) {
  Font font("outline-simple.ttf");
  GFNT_Limits limits{};
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Error error{};
  const std::string path = gfnttest::data("fonts/outline-simple.ttf");

  gfnt_limits_default(&limits);
  limits.max_outline_points = 3;
  ASSERT_EQ(gfnt_blob_create_file(path.c_str(), nullptr, nullptr, &blob,
      &error), GFNT_OK);
  ASSERT_EQ(gfnt_face_load(blob, 0, &limits, nullptr, &face, &error), GFNT_OK);

  GFNT_Outline * outline = nullptr;
  // quad-explicit has four points, which is one more than the cap allows.
  EXPECT_EQ(gfnt_face_glyph_outline(face, kQuadExplicit, nullptr, nullptr,
      &outline, &error), GFNT_ERR_LIMIT);
  EXPECT_NE(error.message, nullptr);
  gfnt_outline_destroy(outline);
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
}

TEST(Glyf, TheContourCapIsAPromiseAndIsEnforced) {
  GFNT_Limits limits{};
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Error error{};
  const std::string path = gfnttest::data("fonts/outline-simple.ttf");

  gfnt_limits_default(&limits);
  limits.max_contours = 1;
  ASSERT_EQ(gfnt_blob_create_file(path.c_str(), nullptr, nullptr, &blob,
      &error), GFNT_OK);
  ASSERT_EQ(gfnt_face_load(blob, 0, &limits, nullptr, &face, &error), GFNT_OK);

  GFNT_Outline * outline = nullptr;
  // A cap of one still reads a one-contour glyph, and refuses the two-contour
  // one beside it. Both halves are the test: a cap that refuses everything is
  // not a cap.
  EXPECT_EQ(gfnt_face_glyph_outline(face, kQuadExplicit, nullptr, nullptr,
      &outline, &error), GFNT_OK) << error.message;
  gfnt_outline_destroy(outline);
  outline = nullptr;
  EXPECT_EQ(gfnt_face_glyph_outline(face, kTwoContours, nullptr, nullptr,
      &outline, &error), GFNT_ERR_LIMIT);
  EXPECT_NE(error.message, nullptr);
  gfnt_outline_destroy(outline);
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
}

TEST(Glyf, TheCompositeDepthCapIsAPromiseAndIsEnforced) {
  GFNT_Limits limits{};
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Error error{};
  const std::string path = gfnttest::data("fonts/outline-composite.ttf");

  gfnt_limits_default(&limits);
  limits.max_composite_depth = 0;
  ASSERT_EQ(gfnt_blob_create_file(path.c_str(), nullptr, nullptr, &blob,
      &error), GFNT_OK);
  ASSERT_EQ(gfnt_face_load(blob, 0, &limits, nullptr, &face, &error), GFNT_OK);

  GFNT_Outline * outline = nullptr;
  // A depth of zero still reads a simple glyph: the cap counts components, and
  // a simple glyph opens none. So the cap has to refuse the composite and
  // allow the glyph beside it, or it is not a depth cap at all.
  EXPECT_EQ(gfnt_face_glyph_outline(face, kQuadExplicit, nullptr, nullptr,
      &outline, &error), GFNT_OK);
  gfnt_outline_destroy(outline);
  outline = nullptr;
  EXPECT_EQ(gfnt_face_glyph_outline(face, kCompOffset, nullptr, nullptr,
      &outline, &error), GFNT_ERR_LIMIT);
  EXPECT_NE(error.message, nullptr);
  gfnt_outline_destroy(outline);
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
}

}  // namespace

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
