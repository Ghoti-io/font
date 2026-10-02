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
 * There is one exception, at the end of the file and marked: a `REPEAT` count
 * that runs past the last point is a font contradicting itself, and no writer
 * will emit one - the same reason `outline-broken-loca.ttf` is a written font
 * with its bytes patched afterwards. Those two cases build the glyph by hand
 * and assert a refusal rather than a shape.
 *
 * documentation/design.md sections 7.3, 14.5 and 14.7.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "sfnt_builder.h"

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

/**
 * One glyph's flag stream, read out of the file's own bytes.
 *
 * A second reading of `glyf`, deliberately: the point of it is to assert what
 * the *input* is rather than what this library made of it. `REPEAT` is a
 * writer's choice - a flag byte and a count stand for a run of points, and
 * nothing obliges a writer to use it - so a test that only checked the decoded
 * points would keep passing if fontTools stopped compressing, and the arm that
 * expands a run would go back to being unreached with the suite still green.
 * That is the shape this test exists to prevent, so it reads the records.
 */
struct FlagStream {
  size_t points = 0;      ///< What `endPtsOfContours` says.
  size_t flag_bytes = 0;  ///< Flag bytes written, repeat counts excluded.
  std::vector<int> repeats;  ///< Each repeat record's count, in order.
};

FlagStream flag_stream_of(const Font & font, uint32_t glyph) {
  FlagStream out;
  const uint8_t * bytes = gfnt_blob_data(font.blob);
  size_t glyf_at = 0;
  size_t glyf_length = 0;
  size_t loca_at = 0;
  size_t loca_length = 0;
  const GFNT_Head * head = nullptr;

  EXPECT_EQ(gfnt_face_table_range(font, GFNT_TAG('g', 'l', 'y', 'f'), &glyf_at,
      &glyf_length), GFNT_OK);
  EXPECT_EQ(gfnt_face_table_range(font, GFNT_TAG('l', 'o', 'c', 'a'), &loca_at,
      &loca_length), GFNT_OK);
  EXPECT_EQ(gfnt_face_head(font, &head, nullptr), GFNT_OK);
  if (!bytes || !head) {
    return out;
  }
  auto u16 = [bytes](size_t at) {
    return (size_t)bytes[at] << 8 | (size_t)bytes[at + 1];
  };
  auto u32 = [bytes](size_t at) {
    return ((size_t)bytes[at] << 24) | ((size_t)bytes[at + 1] << 16)
        | ((size_t)bytes[at + 2] << 8) | (size_t)bytes[at + 3];
  };
  size_t start = 0;
  size_t end = 0;
  if (head->index_to_loc_format == 0) {
    start = u16(loca_at + (size_t)glyph * 2) * 2;
    end = u16(loca_at + ((size_t)glyph + 1) * 2) * 2;
  }
  else {
    start = u32(loca_at + (size_t)glyph * 4);
    end = u32(loca_at + ((size_t)glyph + 1) * 4);
  }
  if (end <= start) {
    return out;  // An empty glyph has no flag stream at all.
  }
  size_t at = glyf_at + start;
  const int contours = (int)(int16_t)(uint16_t)u16(at);
  if (contours <= 0) {
    return out;  // A composite has components rather than points.
  }
  at += 10;
  out.points = u16(at + ((size_t)contours - 1) * 2) + 1;
  at += (size_t)contours * 2;
  at += 2 + u16(at);  // instructionLength, then the instructions
  for (size_t filled = 0; filled < out.points; ) {
    const uint8_t flag = bytes[at];

    at += 1;
    out.flag_bytes += 1;
    filled += 1;
    if (flag & 0x08) {  // REPEAT
      out.repeats.push_back(bytes[at]);
      filled += bytes[at];
      at += 1;
    }
  }
  return out;
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
  kRepeatedFlags = 12,
  kCompOffset = 13,
  kCompWordOffset = 14,
  kCompScale = 15,
  kCompScaledOffset = 16,
  kCompUnscaledOffset = 17,
  kCompXyScale = 18,
  kCompTwoByTwo = 19,
  kCompPointMatch = 20,
  kCompPointMatchScaled = 21,
  kCompNested = 22,
  kCompUseMyMetrics = 23,
  kCompRoundAndOverlap = 24,
  kCompPointMatchHigh = 25,
  kCompInstructions = 26,
};

TEST(Glyf, TheSimpleFixtureHasTheGlyphsItsManifestSays) {
  Font font("outline-simple.ttf");
  size_t glyphs = 0;

  ASSERT_EQ(gfnt_face_num_glyphs(font, &glyphs, nullptr), GFNT_OK);
  EXPECT_EQ(glyphs, 13u);
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

TEST(Glyf, TheRepeatedFlagGlyphIsWrittenWithRepeatRecords) {
  // The input, not the output. Two runs of 259 identical flags, and no repeat
  // count is wider than a byte, so no single record can express either run:
  // whatever split the writer chooses, four records is the fewest it can use.
  // If this ever reads zero, fontTools has stopped compressing and the test
  // below has stopped exercising the expansion it is named for.
  Font font("outline-simple.ttf");
  const FlagStream stream = flag_stream_of(font, kRepeatedFlags);

  EXPECT_EQ(stream.points, 520u);
  EXPECT_GE(stream.repeats.size(), 4u);
  EXPECT_LT(stream.flag_bytes, stream.points);
}

TEST(Glyf, ARepeatedFlagStandsForEveryPointOfItsRun) {
  Font font("outline-simple.ttf");
  Loaded loaded(font, kRepeatedFlags);
  GFNT_Box stated{};

  ASSERT_EQ(loaded.result, GFNT_OK) << loaded.error.message;
  EXPECT_EQ(gfnt_outline_point_count(loaded.outline), 520u);
  EXPECT_EQ(gfnt_outline_contour_count(loaded.outline), 1u);
  // Six flag bytes stand for 520 points, so every coordinate is checked: a
  // reader that expanded a run by one too few or one too many would keep the
  // count right - the coordinate stream says how many there are - and put every
  // later point on the wrong side of the bar.
  for (size_t index = 0; index < 520; ++index) {
    GFNT_Point point{};
    GFNT_PointTag tag = GFNT_POINT_QUAD;
    const int32_t x = index < 260
        ? (int32_t)index * 2
        : 518 - ((int32_t)index - 260) * 2;
    const int32_t y = index < 260 ? 0 : 100;

    ASSERT_EQ(gfnt_outline_point_at(loaded.outline, index, &point, &tag),
        GFNT_OK) << "point " << index;
    EXPECT_EQ(point.x, x * 64) << "point " << index;
    EXPECT_EQ(point.y, y * 64) << "point " << index;
    EXPECT_EQ(tag, GFNT_POINT_ON) << "point " << index;
  }
  ASSERT_EQ(gfnt_face_glyph_stated_box(font, kRepeatedFlags, &stated, nullptr),
      GFNT_OK);
  EXPECT_EQ(box_string(stated), "0 0 518 100");
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
  EXPECT_EQ(glyphs, 27u);
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

TEST(Glyf, AFaceWhoseOutlinesAreCharstringsIsAnsweredByTheOtherProducer) {
  Font font("cff.otf");
  Loaded loaded(font, 2);

  // **Every assertion here was the opposite until phase 2**, and each was right
  // then: an OTTO face had outlines in a table nothing could read, so asking
  // refused with a message saying which table and which phase. Kept and flipped
  // rather than deleted, because a test that asserts something is unsupported
  // goes on passing the day it becomes supported and says nothing.
  EXPECT_TRUE(gfnt_face_has_outlines(font));
  EXPECT_TRUE(gfnt_face_has_table(font, GFNT_TAG('C', 'F', 'F', ' ')));
  EXPECT_EQ(loaded.result, GFNT_OK) << loaded.error.message;
  EXPECT_GT(gfnt_outline_point_count(loaded.outline), 0u);

  // `glyf`'s own two accessors answer for a charstring face as well, and one of
  // them has nothing to answer with: a CFF glyph states no bounding box, so the
  // refusal names that rather than pretending the font's FontBBox is this
  // glyph's.
  bool composite = false;
  GFNT_Error error{};
  EXPECT_EQ(gfnt_face_glyph_is_composite(font, 2, &composite, &error), GFNT_OK)
      << error.message;
  EXPECT_FALSE(composite);

  GFNT_Box box{};
  EXPECT_EQ(gfnt_face_glyph_stated_box(font, 2, &box, &error),
      GFNT_ERR_UNSUPPORTED);
  ASSERT_NE(error.message, nullptr);
  EXPECT_NE(std::string(error.message).find("FontBBox"), std::string::npos)
      << error.message;
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
  // sweep walks a nesting as well as a growth. How many requests that is, is
  // measured rather than guessed: a ceiling short of the last allocation is how
  // a sweep stops covering whatever somebody adds next.
  for (size_t glyph : {(size_t)kCompNested, (size_t)kManyPoints}) {
    size_t requests = 0;
    {
      gfnttest::FailingAllocator counter(static_cast<size_t>(-1));
      GFNT_Outline * outline = nullptr;

      ASSERT_EQ(gfnt_face_glyph_outline(font, (uint32_t)glyph, nullptr,
          counter.get(), &outline, nullptr), GFNT_OK);
      requests = counter.requests();
      gfnt_outline_destroy(outline);
      EXPECT_GE(requests, 4u) << "glyph " << glyph;
    }

    for (size_t fail_at = 0; fail_at <= requests; ++fail_at) {
      gfnttest::FailingAllocator allocator(fail_at, 0);
      GFNT_Outline * outline = nullptr;
      GFNT_Error error{};
      GFNT_Result result = gfnt_face_glyph_outline(font, (uint32_t)glyph,
          nullptr, allocator.get(), &outline, &error);

      if (result != GFNT_OK) {
        EXPECT_EQ(result, GFNT_ERR_OOM) << "at request " << fail_at;
        EXPECT_EQ(outline, nullptr);
        EXPECT_NE(error.message, nullptr);
      }
      gfnt_outline_destroy(outline);
      EXPECT_EQ(allocator.live(), 0u) << "at request " << fail_at;
    }
  }
}

TEST(Glyf, TheStatedBoxOfACondemnedGlyphIsRefusedRatherThanZero) {
  // The box comes from the glyph's own bytes, so a `loca` entry that condemns
  // the glyph condemns its box too. Zeroes would be a claim the font did not
  // make - which is the same reason an empty glyph reports an empty box rather
  // than a zero one.
  Font font("outline-broken-loca.ttf");
  size_t glyphs = 0;
  GFNT_Box box{};
  GFNT_Error error{};

  ASSERT_EQ(gfnt_face_num_glyphs(font, &glyphs, nullptr), GFNT_OK);
  EXPECT_EQ(gfnt_face_glyph_stated_box(font, (uint32_t)(glyphs - 1), &box,
      &error), GFNT_ERR_CORRUPT);
  ASSERT_NE(error.message, nullptr);

  // And the glyph before it still answers, which is what makes the refusal about
  // one glyph rather than about the font.
  EXPECT_EQ(gfnt_face_glyph_stated_box(font, (uint32_t)(glyphs - 2), &box,
      &error), GFNT_OK) << error.message;
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

// ---------------------------------------------------------------------------
// The one encoding fontTools will not write.

/** A one-contour glyph whose flag stream is given literally, `REPEAT` and all. */
std::vector<uint8_t> flag_stream_glyph(size_t points,
    const std::vector<uint8_t> & flags) {
  std::vector<uint8_t> out;

  gfnttest::put_s16(out, 1);            // one contour
  gfnttest::put_s16(out, 0);            // xMin
  gfnttest::put_s16(out, 0);            // yMin
  gfnttest::put_s16(out, 400);          // xMax
  gfnttest::put_s16(out, 100);          // yMax
  gfnttest::put_u16(out, (uint16_t)(points - 1));  // endPtsOfContours
  gfnttest::put_u16(out, 0);            // instructionLength
  out.insert(out.end(), flags.begin(), flags.end());
  // No flag here sets X_SHORT or Y_SHORT, so every delta is a signed word.
  for (size_t index = 0; index < points; ++index) {
    gfnttest::put_s16(out, 100);
  }
  for (size_t index = 0; index < points; ++index) {
    gfnttest::put_s16(out, index % 2 == 0 ? 100 : -100);
  }
  return out;
}

/** A font over the given glyph descriptions, with `loca` built to match. */
std::vector<uint8_t> font_over(const std::vector<std::vector<uint8_t>> & glyphs) {
  std::vector<uint8_t> glyf;
  std::vector<uint8_t> loca;
  bool long_loca = false;
  std::vector<std::pair<uint16_t, int16_t>> metrics;

  gfnttest::build_glyf_and_loca(glyphs, &glyf, &loca, &long_loca);
  for (size_t i = 0; i < glyphs.size(); ++i) {
    metrics.push_back({(uint16_t)(500 + 10 * i), (int16_t)i});
  }
  return gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, {
      {GFNT_TAG('h', 'e', 'a', 'd'),
          gfnttest::build_head(1000, long_loca ? 1 : 0)},
      {GFNT_TAG('h', 'h', 'e', 'a'),
          gfnttest::build_hhea(800, -200, 100, (uint16_t)glyphs.size())},
      {GFNT_TAG('h', 'm', 't', 'x'), gfnttest::build_hmtx(metrics, {})},
      {GFNT_TAG('m', 'a', 'x', 'p'),
          gfnttest::build_maxp((uint16_t)glyphs.size())},
      {GFNT_TAG('g', 'l', 'y', 'f'), glyf},
      {GFNT_TAG('l', 'o', 'c', 'a'), loca},
  });
}

/** A font whose glyph 1 is that glyph, and whose glyph 0 is empty. */
std::vector<uint8_t> font_with(const std::vector<uint8_t> & glyph) {
  return font_over({{}, glyph});
}

/**
 * The same, with `loca` describing each glyph's length **exactly**.
 *
 * ::gfnttest::build_glyf_and_loca() pads every glyph to an even length, which is
 * what a writer does - and it makes a length sweep measure the wrong thing: a
 * description cut to 53 bytes is stored as 54 with a zero at the end, so the
 * reader is handed a complete glyph whose last coordinate differs, reports
 * success, and the sweep records a misread that is really the builder's. So this
 * writes a long `loca` with the offsets the caller asked for, odd ones included.
 */
std::vector<uint8_t> font_over_exact(
    const std::vector<std::vector<uint8_t>> & glyphs) {
  std::vector<uint8_t> glyf;
  std::vector<uint8_t> loca;
  std::vector<std::pair<uint16_t, int16_t>> metrics;

  for (const std::vector<uint8_t> & glyph : glyphs) {
    gfnttest::put_u32(loca, (uint32_t)glyf.size());
    glyf.insert(glyf.end(), glyph.begin(), glyph.end());
  }
  gfnttest::put_u32(loca, (uint32_t)glyf.size());
  for (size_t i = 0; i < glyphs.size(); ++i) {
    metrics.push_back({(uint16_t)(500 + 10 * i), (int16_t)i});
  }
  return gfnttest::build_sfnt(GFNT_FLAVOUR_TRUETYPE, {
      {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(1000, 1)},
      {GFNT_TAG('h', 'h', 'e', 'a'),
          gfnttest::build_hhea(800, -200, 100, (uint16_t)glyphs.size())},
      {GFNT_TAG('h', 'm', 't', 'x'), gfnttest::build_hmtx(metrics, {})},
      {GFNT_TAG('m', 'a', 'x', 'p'),
          gfnttest::build_maxp((uint16_t)glyphs.size())},
      {GFNT_TAG('g', 'l', 'y', 'f'), glyf},
      {GFNT_TAG('l', 'o', 'c', 'a'), loca},
  });
}

/**
 * A one-contour glyph whose coordinates are single bytes.
 *
 * ::gfnttest::build_glyf_glyph() writes every delta as a signed word, which is
 * the encoding a reader must accept rather than the one a writer would choose.
 * This is the other one: `X_SHORT` and `Y_SHORT` with the same-or-positive bits,
 * so each coordinate is one unsigned byte, and a cut inside those bytes reaches
 * the short read rather than the wide one.
 */
std::vector<uint8_t> short_coordinate_glyph() {
  std::vector<uint8_t> out;
  const uint8_t flag = 0x01u | 0x02u | 0x04u | 0x10u | 0x20u;

  gfnttest::put_s16(out, 1);            // one contour
  gfnttest::put_s16(out, 0);            // xMin
  gfnttest::put_s16(out, 0);            // yMin
  gfnttest::put_s16(out, 90);           // xMax
  gfnttest::put_s16(out, 90);           // yMax
  gfnttest::put_u16(out, 3);            // endPtsOfContours: four points
  gfnttest::put_u16(out, 0);            // instructionLength
  for (int i = 0; i < 4; ++i) {
    out.push_back(flag);
  }
  for (int i = 0; i < 4; ++i) {
    out.push_back(30);                  // x deltas, one byte each
  }
  for (int i = 0; i < 4; ++i) {
    out.push_back(20);                  // y deltas
  }
  return out;
}

/** A simple glyph with two contours and instructions: something to cut. */
std::vector<uint8_t> two_contour_glyph() {
  return gfnttest::build_glyf_glyph({
      {{0, 0, true}, {300, 0, true}, {300, 300, false}, {0, 300, true}},
      {{80, 80, true}, {200, 80, true}, {200, 200, true}},
  }, {0x00, 0x01, 0x02});
}

/**
 * Load glyph @p glyph of a font built from @p glyphs, and say what happened.
 *
 * The result is what the sweeps below compare; nothing here asserts, because a
 * refusal is the expected answer for most of the inputs they build.
 */
struct Outcome {
  GFNT_Result result = GFNT_ERR_INTERNAL;
  size_t points = 0;
  size_t contours = 0;
  const char * message = nullptr;

  Outcome(const std::vector<uint8_t> & bytes, uint32_t glyph) {
    GFNT_Blob * blob = nullptr;
    GFNT_Face * face = nullptr;
    GFNT_Outline * outline = nullptr;
    GFNT_Error error{};

    gfnt_error_clear(&error);
    EXPECT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(),
        GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr), GFNT_OK);
    if (gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error) == GFNT_OK) {
      result = gfnt_face_glyph_outline(face, glyph, nullptr, nullptr, &outline,
          &error);
      if (result == GFNT_OK) {
        points = gfnt_outline_point_count(outline);
        contours = gfnt_outline_contour_count(outline);
      }
      message = error.message;
    }
    gfnt_outline_destroy(outline);
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
};

/** Load glyph 1 of such a font, and say what happened. */
struct HandBuilt {
  std::vector<uint8_t> bytes;
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Outline * outline = nullptr;
  GFNT_Error error{};
  GFNT_Result result = GFNT_ERR_INTERNAL;

  explicit HandBuilt(const std::vector<uint8_t> & glyph)
      : bytes(font_with(glyph)) {
    gfnt_error_clear(&error);
    EXPECT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(),
        GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr), GFNT_OK);
    EXPECT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error), GFNT_OK)
        << (error.message ? error.message : "");
    result = gfnt_face_glyph_outline(face, 1, nullptr, nullptr, &outline,
        &error);
  }
  ~HandBuilt() {
    gfnt_outline_destroy(outline);
    gfnt_face_free(face);
    gfnt_blob_destroy(blob);
  }
  HandBuilt(const HandBuilt &) = delete;
  HandBuilt & operator=(const HandBuilt &) = delete;
};

TEST(Glyf, ARepeatCountInsideTheContourFillsExactlyItsRun) {
  // The control, and it is the half that can fail for a reason that has nothing
  // to do with the count: hand-built bytes this reader refuses for some other
  // reason would make the refusal below prove nothing. Flag 0x09 is an on-curve
  // point with REPEAT, and a count of 3 describes the contour's other three.
  HandBuilt built(flag_stream_glyph(4, {0x09, 3}));

  ASSERT_EQ(built.result, GFNT_OK) << built.error.message;
  EXPECT_EQ(gfnt_outline_point_count(built.outline), 4u);
  EXPECT_EQ(gfnt_outline_contour_count(built.outline), 1u);
}

TEST(Glyf, ARepeatCountPastTheLastPointIsRefusedRatherThanFilledAsFarAsItFits) {
  // The same glyph, with the count alone changed: 250 more points in a contour
  // that has four. Filling only as many as fit would be this library inventing
  // an interpretation, and the rest of the stream is then read as coordinates
  // of points the font never described.
  HandBuilt built(flag_stream_glyph(4, {0x09, 250}));

  EXPECT_EQ(built.result, GFNT_ERR_CORRUPT);
  EXPECT_EQ(built.error.glyph, 1u);
  EXPECT_EQ(built.error.table, GFNT_TAG('g', 'l', 'y', 'f'));
  EXPECT_NE(built.error.message, nullptr);
}

TEST(Glyf, AGlyphDescriptionCutToEveryLengthIsRefusedRatherThanMisread) {
  // The whole-font truncation sweep cannot reach any of this. It cuts a table's
  // extent, and `loca` is then checked against the shortened `glyf` before a
  // description is read at all - so every cut is refused by the range check and
  // the reader inside a glyph never sees a short buffer. What does is a `loca`
  // that agrees with `glyf` and a *description* too short for what its own
  // header says, which is a font no writer produces and a fuzzer finds at once.
  // Two glyphs, because a coordinate is read as a byte or as a word depending on
  // its flag and the builder writes only words: the short encoding is most of
  // what a real font uses, and a sweep over words alone leaves its read unwalked.
  const std::vector<uint8_t> word_glyph = two_contour_glyph();
  const std::vector<uint8_t> byte_glyph = short_coordinate_glyph();
  size_t refusals = 0;

  for (const std::vector<uint8_t> & whole : {word_glyph, byte_glyph}) {
  for (size_t cut = 0; cut < whole.size(); ++cut) {
    const std::vector<uint8_t> prefix(whole.begin(), whole.begin() + cut);
    const Outcome outcome(font_over_exact({{}, prefix}), 1);

    if (cut == 0) {
      // Zero bytes is not a cut glyph, it is an empty one: `loca` saying a
      // glyph has no description is how every space in every font is spelled.
      EXPECT_EQ(outcome.result, GFNT_OK) << "cut to " << cut;
      EXPECT_EQ(outcome.points, 0u);
      continue;
    }
    EXPECT_NE(outcome.result, GFNT_OK) << "cut to " << cut;
    EXPECT_EQ(outcome.result, GFNT_ERR_CORRUPT) << "cut to " << cut;
    EXPECT_NE(outcome.message, nullptr) << "cut to " << cut;
    refusals++;
  }
  // The control: uncut, the same font reads the glyph the builder described.
  const Outcome full(font_over_exact({{}, whole}), 1);
  EXPECT_EQ(full.result, GFNT_OK) << full.message;
  EXPECT_GT(full.points, 0u);
  }
  EXPECT_EQ(refusals, word_glyph.size() + byte_glyph.size() - 2);
}

TEST(Glyf, ContourEndsThatDoNotIncreaseAreRefusedRatherThanReadAsEmpty) {
  // endPtsOfContours is the *last* point of each contour, so it increases. A
  // second entry at or below the first describes a contour with no points, which
  // is not the same thing as a one-point contour and cannot be drawn: the later
  // contour would own a negative number of points.
  std::vector<uint8_t> glyph;

  gfnttest::put_s16(glyph, 2);          // two contours
  gfnttest::put_s16(glyph, 0);
  gfnttest::put_s16(glyph, 0);
  gfnttest::put_s16(glyph, 100);
  gfnttest::put_s16(glyph, 100);
  gfnttest::put_u16(glyph, 5);          // the first ends at point 5
  gfnttest::put_u16(glyph, 2);          // and the second, at point 2
  gfnttest::put_u16(glyph, 0);          // instructionLength
  for (int i = 0; i < 6; ++i) {
    glyph.push_back(0x01u);             // on-curve, wide coordinates
  }
  for (int i = 0; i < 12; ++i) {
    gfnttest::put_s16(glyph, 10);
  }
  const Outcome outcome(font_over_exact({{}, glyph}), 1);

  EXPECT_EQ(outcome.result, GFNT_ERR_CORRUPT);
  ASSERT_NE(outcome.message, nullptr);
  EXPECT_NE(std::string(outcome.message).find("does not increase"),
      std::string::npos) << outcome.message;
}

TEST(Glyf, ACompositeCutToEveryLengthIsRefusedRatherThanMisread) {
  // The same sweep one level up: a composite's bytes are a component record at a
  // time, and a cut inside one lands in the middle of a flag word, a glyph
  // index, an argument or a transform. Each of those is a separate read and a
  // separate arm.
  const std::vector<uint8_t> simple = gfnttest::build_glyf_glyph(
      {{{0, 0, true}, {100, 0, true}, {100, 100, true}}});
  const std::vector<uint8_t> whole = gfnttest::build_glyf_composite({
      {1, 0x0003, 40, 60, {}},                        // words, xy values
      {1, 0x0002 | 0x0008, 5, 7, {0x4000}},           // signed bytes, one scale
      // Byte-sized *point indices*: a third argument encoding, and the one whose
      // two bytes are unsigned. Without a component using it, the cuts that land
      // in it could not exist.
      {1, 0x0000, 1, 2, {}},
      {1, 0x0003 | 0x0040, -20, 30, {0x4000, 0x4000}},          // x and y
      {1, 0x0003 | 0x0080, 10, 10, {0x4000, 0, 0, 0x4000}},     // two-by-two
  }, {0x00, 0x01});
  size_t refusals = 0;

  const Outcome full(font_over_exact({{}, simple, whole}), 2);
  ASSERT_EQ(full.result, GFNT_OK) << full.message;
  ASSERT_EQ(full.points, 15u) << "three points per component, five components";
  ASSERT_EQ(full.contours, 5u);

  for (size_t cut = 1; cut < whole.size(); ++cut) {
    const std::vector<uint8_t> prefix(whole.begin(), whole.begin() + cut);
    const Outcome outcome(font_over_exact({{}, simple, prefix}), 2);

    if (outcome.result == GFNT_OK) {
      // Property one rather than "everything refuses": the four bytes at the
      // end are this composite's instructions, and nothing reads a composite's
      // instructions - hinting is skipped by length, never run (design.md
      // section 8.5). Cutting bytes no reader looks at changes no answer, which
      // is the other half of the property and not an exception to it.
      EXPECT_EQ(outcome.points, full.points) << "cut to " << cut;
      EXPECT_EQ(outcome.contours, full.contours) << "cut to " << cut;
      continue;
    }
    EXPECT_NE(outcome.message, nullptr) << "cut to " << cut;
    refusals++;
  }
  // Every cut inside a component record, and none of the four inside the
  // instructions the reader steps over.
  EXPECT_EQ(refusals, whole.size() - 5);
}

TEST(Glyf, ACycleIsNamedRatherThanLeftToTheDepthCap) {
  // Left to the depth cap any of these reports a limit, which is true and
  // useless: the font is not deep, it is circular - and a caller acting on a
  // limit raises the budget and gets the same answer forever. No writer emits
  // one, so the bytes are built here.
  //
  // The three-glyph case is the one that decides the shape of the check: a
  // component compared only against the composite it sits in, or only against
  // the one that opened the chain, refuses the first two and walks the third to
  // the cap. The chain of every glyph currently open is what catches all three.
  struct Case {
    const char * why;
    std::vector<uint16_t> targets;   // glyph i + 1 draws targets[i]
  };
  const Case cases[] = {
    {"a composite that includes itself", {1}},
    {"a cycle through another glyph", {2, 1}},
    {"a cycle that closes two glyphs later", {2, 3, 1}},
  };

  for (const Case & test : cases) {
    std::vector<std::vector<uint8_t>> glyphs = {{}};

    for (uint16_t target : test.targets) {
      glyphs.push_back(gfnttest::build_glyf_composite({
          {target, 0x0003, 0, 0},
      }));
    }
    const Outcome outcome(font_over(glyphs), 1);

    EXPECT_EQ(outcome.result, GFNT_ERR_CORRUPT) << test.why;
    EXPECT_NE(std::string(outcome.message ? outcome.message : "").find(
        "includes itself"), std::string::npos) << test.why << ": got "
        << (outcome.message ? outcome.message : "(nothing)");
  }
}

TEST(Glyf, TheSameComponentTwiceIsNotACycle) {
  // The other half of the cycle check, and the regression it is one line away
  // from: a *visited set* would refuse this, because the chain and a visited set
  // differ only on a glyph reached twice without either reach containing the
  // other. Every diacritic font does it - "ä" is `a` and two copies of one dot -
  // so refusing it would refuse real fonts while still passing the cycle tests.
  //
  // Siblings and a diamond both, because they fail differently: a set cleared
  // per component would still refuse the diamond.
  const std::vector<uint8_t> simple = gfnttest::build_glyf_glyph(
      {{{0, 0, true}, {100, 0, true}, {100, 100, true}}});
  // Glyph 2 draws glyph 1 twice; glyph 3 draws glyph 2 and glyph 1, so glyph 1
  // is reached down two different paths.
  const std::vector<uint8_t> siblings = gfnttest::build_glyf_composite({
      {1, 0x0003, 0, 0},
      {1, 0x0003, 200, 0},
  });
  const std::vector<uint8_t> diamond = gfnttest::build_glyf_composite({
      {2, 0x0003, 0, 0},
      {1, 0x0003, 0, 300},
  });

  const Outcome twice(font_over({{}, simple, siblings, diamond}), 2);
  EXPECT_EQ(twice.result, GFNT_OK) << (twice.message ? twice.message : "");
  EXPECT_EQ(twice.points, 6u) << "three points, drawn twice";
  EXPECT_EQ(twice.contours, 2u);

  const Outcome both(font_over({{}, simple, siblings, diamond}), 3);
  EXPECT_EQ(both.result, GFNT_OK) << (both.message ? both.message : "");
  EXPECT_EQ(both.points, 9u) << "glyph 2's two copies, and a third beside them";
  EXPECT_EQ(both.contours, 3u);
}

TEST(Glyf, AnAcyclicChainDeeperThanTheBudgetIsStillALimit) {
  // What is left for `max_composite_depth` once a cycle is named: a font that is
  // deep rather than circular, which is ::GFNT_ERR_LIMIT and not corrupt because
  // the ceiling is the caller's and raising it is an action they can take. The
  // cap test above sets the budget to zero, which a cycle check cannot be
  // confused with; this one nests below a budget of two and is the case that
  // separates the two refusals.
  GFNT_Limits limits{};
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Outline * outline = nullptr;
  GFNT_Error error{};
  // Glyph 1 is the leaf; glyphs 2..5 each draw the one below, so glyph 5 is four
  // composite levels above it - and every glyph index is distinct, so nothing
  // here repeats, let alone cycles.
  std::vector<std::vector<uint8_t>> glyphs = {{}, gfnttest::build_glyf_glyph(
      {{{0, 0, true}, {100, 0, true}, {100, 100, true}}})};
  for (uint16_t glyph = 1; glyph <= 4; ++glyph) {
    glyphs.push_back(gfnttest::build_glyf_composite({
        {glyph, 0x0003, 0, 0},
    }));
  }
  const std::vector<uint8_t> bytes = font_over(glyphs);

  gfnt_limits_default(&limits);
  limits.max_composite_depth = 2;
  ASSERT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(),
      GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_face_load(blob, 0, &limits, nullptr, &face, &error), GFNT_OK);

  // Two levels is inside the budget and three is not, which is both halves: a
  // cap that refuses the shallow chain as well would not be measuring depth.
  EXPECT_EQ(gfnt_face_glyph_outline(face, 3, nullptr, nullptr, &outline,
      &error), GFNT_OK) << (error.message ? error.message : "");
  gfnt_outline_destroy(outline);
  outline = nullptr;
  EXPECT_EQ(gfnt_face_glyph_outline(face, 5, nullptr, nullptr, &outline,
      &error), GFNT_ERR_LIMIT);
  ASSERT_NE(error.message, nullptr);
  EXPECT_NE(std::string(error.message).find("nest deeper"), std::string::npos)
      << error.message;
  gfnt_outline_destroy(outline);
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
}

TEST(Glyf, AComponentMatchingAPointNeitherGlyphHasIsRefused) {
  // Point matching without ARGS_ARE_XY_VALUES: arg1 indexes the composite so
  // far and arg2 the component. Guessing zero for an index that is not there
  // would stack the component on the origin and draw a glyph that looks almost
  // right, which is the outcome worth refusing.
  const std::vector<uint8_t> simple = gfnttest::build_glyf_glyph(
      {{{0, 0, true}, {100, 0, true}, {100, 100, true}}});
  const std::vector<uint8_t> matched = gfnttest::build_glyf_composite({
      {1, 0x0000, 0, 0},      // the first component anchors the composite
      {1, 0x0000, 90, 1},     // and this one matches a point that is not there
  });
  const Outcome outcome(font_over_exact({{}, simple, matched}), 2);

  EXPECT_EQ(outcome.result, GFNT_ERR_CORRUPT);
  ASSERT_NE(outcome.message, nullptr);
  EXPECT_NE(std::string(outcome.message).find("point neither glyph has"),
      std::string::npos) << outcome.message;
}

TEST(Glyf, AnIndexToLocFormatOtherThanZeroOrOneIsRefused) {
  // `head` has two loca formats and a font that names a third is broken in a
  // way that decides the width of every offset in the table. Read as either
  // width it would give offsets that are not the font's.
  std::vector<uint8_t> glyf;
  std::vector<uint8_t> loca;
  bool long_loca = false;
  gfnttest::build_glyf_and_loca({{}, two_contour_glyph()}, &glyf, &loca,
      &long_loca);
  const std::vector<uint8_t> bytes = gfnttest::build_sfnt(
      GFNT_FLAVOUR_TRUETYPE, {
          {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(1000, 2)},
          {GFNT_TAG('h', 'h', 'e', 'a'), gfnttest::build_hhea(800, -200, 0, 2)},
          {GFNT_TAG('h', 'm', 't', 'x'),
              gfnttest::build_hmtx({{500, 0}, {500, 0}}, {})},
          {GFNT_TAG('m', 'a', 'x', 'p'), gfnttest::build_maxp(2)},
          {GFNT_TAG('g', 'l', 'y', 'f'), glyf},
          {GFNT_TAG('l', 'o', 'c', 'a'), loca},
      });
  const Outcome outcome(bytes, 1);

  EXPECT_EQ(outcome.result, GFNT_ERR_CORRUPT);
  ASSERT_NE(outcome.message, nullptr);
  EXPECT_NE(std::string(outcome.message).find("indexToLocFormat"),
      std::string::npos) << outcome.message;
}

TEST(Glyf, ALocaWithFewerEntriesThanGlyphsRefusesThoseGlyphsAndNotTheFont) {
  // `loca` holds numGlyphs+1 offsets and this one holds two, in the long form so
  // that each is four bytes and the cut is unambiguous. Glyph 0 is still
  // readable - its pair of offsets is there - and glyph 1's is not.
  //
  // `loca` does not join the numGlyphs minimum (M12 is about tables that *index*
  // glyphs by position, and metrics.c says which those are), so this font says
  // it has two glyphs and can only answer for one. That is M11's rule: the glyph
  // is condemned, the font is not.
  const std::vector<uint8_t> glyph = two_contour_glyph();
  std::vector<uint8_t> glyf = glyph;
  std::vector<uint8_t> loca;

  gfnttest::put_u32(loca, 0);
  gfnttest::put_u32(loca, (uint32_t)glyph.size());
  const std::vector<uint8_t> bytes = gfnttest::build_sfnt(
      GFNT_FLAVOUR_TRUETYPE, {
          {GFNT_TAG('h', 'e', 'a', 'd'), gfnttest::build_head(1000, 1)},
          {GFNT_TAG('h', 'h', 'e', 'a'), gfnttest::build_hhea(800, -200, 0, 2)},
          {GFNT_TAG('h', 'm', 't', 'x'),
              gfnttest::build_hmtx({{500, 0}, {500, 0}}, {})},
          {GFNT_TAG('m', 'a', 'x', 'p'), gfnttest::build_maxp(2)},
          {GFNT_TAG('g', 'l', 'y', 'f'), glyf},
          {GFNT_TAG('l', 'o', 'c', 'a'), loca},
      });

  const Outcome first(bytes, 0);
  EXPECT_EQ(first.result, GFNT_OK) << first.message;
  EXPECT_EQ(first.points, 7u);

  const Outcome second(bytes, 1);
  EXPECT_NE(second.result, GFNT_OK);
  EXPECT_NE(second.message, nullptr);
}

TEST(Glyf, EveryGlyphAccessorReportsTheSameCorruptionRatherThanOneOfThem) {
  // Three accessors read the same description - the outline, whether it is a
  // composite, and the box the font states - and each has its own propagation to
  // do. A caller that asked the cheap question first would otherwise be told the
  // glyph is fine.
  const std::vector<uint8_t> whole = two_contour_glyph();
  // Glyph 1 keeps its ten-byte header and loses everything after it; glyph 2 does
  // not even have that. The header is all these two accessors read, so the first
  // still answers and the second cannot - which is the line between them, and it
  // is not where the outline's is.
  const std::vector<uint8_t> header(whole.begin(), whole.begin() + 12);
  const std::vector<uint8_t> stub(whole.begin(), whole.begin() + 1);
  const std::vector<uint8_t> bytes = font_over_exact({{}, header, stub});
  GFNT_Blob * blob = nullptr;
  GFNT_Face * face = nullptr;
  GFNT_Error error{};
  bool composite = true;
  GFNT_Box box{};
  GFNT_Outline * outline = nullptr;

  ASSERT_EQ(gfnt_blob_create_memory(bytes.data(), bytes.size(),
      GFNT_BLOB_BORROWED, nullptr, nullptr, &blob, nullptr), GFNT_OK);
  ASSERT_EQ(gfnt_face_load(blob, 0, nullptr, nullptr, &face, &error), GFNT_OK);

  EXPECT_EQ(gfnt_face_glyph_is_composite(face, 1, &composite, &error), GFNT_OK);
  EXPECT_FALSE(composite);
  EXPECT_EQ(gfnt_face_glyph_stated_box(face, 1, &box, &error), GFNT_OK);
  EXPECT_EQ(box_string(box), "0 0 300 300");
  EXPECT_EQ(gfnt_face_glyph_outline(face, 1, nullptr, nullptr, &outline, &error),
      GFNT_ERR_CORRUPT) << "the points are what is missing";
  gfnt_outline_destroy(outline);
  outline = nullptr;

  EXPECT_EQ(gfnt_face_glyph_is_composite(face, 2, &composite, &error),
      GFNT_ERR_CORRUPT);
  EXPECT_EQ(gfnt_face_glyph_stated_box(face, 2, &box, &error), GFNT_ERR_CORRUPT);

  // And the null arguments each accessor has to refuse before it reads anything.
  EXPECT_EQ(gfnt_face_glyph_is_composite(nullptr, 0, &composite, &error),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_glyph_is_composite(face, 0, nullptr, &error),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_glyph_stated_box(nullptr, 0, &box, &error),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_glyph_stated_box(face, 0, nullptr, &error),
      GFNT_ERR_INVALID);
  gfnt_face_free(face);
  gfnt_blob_destroy(blob);
}

TEST(Glyf, ADescriptionStatingZeroContoursIsAnEmptyGlyphAndNotAnError) {
  // Two bytes that say "no contours". A glyph with no description at all is the
  // ordinary empty glyph; this one has a description and it is empty, and fonts
  // contain both. Reading the first as a refusal would lose a space; reading the
  // second as one would lose a glyph that draws nothing on purpose.
  const Outcome stated(font_over_exact({{}, {0x00, 0x00}}), 1);

  EXPECT_EQ(stated.result, GFNT_OK) << stated.message;
  EXPECT_EQ(stated.points, 0u);
  EXPECT_EQ(stated.contours, 0u);
}

TEST(Glyf, TheFacesOwnAllocatorIsSweptToo) {
  // The sweep above refuses the *caller's* allocator, which pays for the outline.
  // The scratch a parse needs - the flag stream, and the outline a component is
  // assembled into - comes from the allocator the face was loaded with, so a
  // sweep that only varies the caller's never refuses any of it.
  const std::string path = gfnttest::data("fonts/outline-composite.ttf");
  GFNT_Blob * blob = nullptr;
  size_t requests = 0;

  ASSERT_EQ(gfnt_blob_create_file(path.c_str(), nullptr, nullptr, &blob,
      nullptr), GFNT_OK);
  {
    gfnttest::FailingAllocator counter(static_cast<size_t>(-1));
    GFNT_Face * face = nullptr;
    GFNT_Outline * outline = nullptr;

    ASSERT_EQ(gfnt_face_load(blob, 0, nullptr, counter.get(), &face, nullptr),
        GFNT_OK);
    ASSERT_EQ(gfnt_face_glyph_outline(face, kCompNested, nullptr, nullptr,
        &outline, nullptr), GFNT_OK);
    gfnt_outline_destroy(outline);
    outline = nullptr;
    ASSERT_EQ(gfnt_face_glyph_outline(face, kManyPoints, nullptr, nullptr,
        &outline, nullptr), GFNT_OK);
    requests = counter.requests();
    gfnt_outline_destroy(outline);
    gfnt_face_free(face);
    EXPECT_GT(requests, 6u);
  }

  for (size_t fail_at = 0; fail_at <= requests; ++fail_at) {
    gfnttest::FailingAllocator allocator(fail_at, 0);
    GFNT_Face * face = nullptr;
    GFNT_Error error{};

    if (gfnt_face_load(blob, 0, nullptr, allocator.get(), &face, &error)
        != GFNT_OK) {
      EXPECT_EQ(error.result, GFNT_ERR_OOM) << "at request " << fail_at;
      EXPECT_EQ(allocator.live(), 0u) << "at request " << fail_at;
      continue;
    }
    for (uint32_t glyph : {(uint32_t)kCompNested, (uint32_t)kManyPoints}) {
      GFNT_Outline * outline = nullptr;
      const GFNT_Result result = gfnt_face_glyph_outline(face, glyph, nullptr,
          nullptr, &outline, &error);

      if (result != GFNT_OK) {
        EXPECT_EQ(result, GFNT_ERR_OOM) << "at request " << fail_at;
        EXPECT_EQ(outline, nullptr) << "at request " << fail_at;
        EXPECT_NE(error.message, nullptr) << "at request " << fail_at;
      }
      gfnt_outline_destroy(outline);
    }
    allocator.stop_failing();
    gfnt_face_free(face);
    EXPECT_EQ(allocator.live(), 0u) << "at request " << fail_at;
  }
  gfnt_blob_destroy(blob);
}

}  // namespace

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
