/**
 * @file
 *
 * `GFNT_Outline` on its own: storage, transforms, bounds, and the path walk.
 *
 * These expectations are **not** read off a font. Every point here is written
 * out by hand, so the walk's rules - the implicit on-curve point between two
 * off-curve points, a contour that begins off-curve, a contour with nothing
 * on-curve at all - are asserted against the specification rather than against
 * whatever `glyf` happened to hand over. `test_glyf.cpp` is the other half and
 * reads fonts fontTools wrote.
 *
 * The cubic arm is here and nowhere else. Nothing in this library produces a
 * cubic yet - charstrings are phase 2 - so without a hand-built cubic outline
 * `cubic_to`, the two-control walk and the cubic bound would be code that has
 * never run, which is how an unbuilt branch comes to look finished.
 *
 * documentation/design.md sections 7.3 and 8.1.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <string>
#include <vector>

#include <ghoti.io/font/outline.h>

namespace {

/** 26.6 for a whole number, which every coordinate here is. */
constexpr GFNT_F26Dot6 u(int32_t whole) {
  return static_cast<GFNT_F26Dot6>(whole * GFNT_F26DOT6_ONE);
}

/** One point of a contour under construction. */
struct Pt {
  int32_t x;
  int32_t y;
  GFNT_PointTag tag;
};

/** An outline owned for the length of a test. */
struct Outline {
  GFNT_Outline * handle = nullptr;
  GFNT_Error error{};

  Outline() {
    EXPECT_EQ(gfnt_outline_create(nullptr, &handle, &error), GFNT_OK);
  }
  ~Outline() { gfnt_outline_destroy(handle); }
  Outline(const Outline &) = delete;
  Outline & operator=(const Outline &) = delete;

  /** Add one contour from its points. */
  void contour(const std::vector<Pt> & points) {
    ASSERT_EQ(gfnt_outline_begin_contour(handle, &error), GFNT_OK);
    for (const Pt & point : points) {
      GFNT_Point where{u(point.x), u(point.y)};
      ASSERT_EQ(gfnt_outline_add_point(handle, where, point.tag, &error),
          GFNT_OK) << error.message;
    }
  }

  operator GFNT_Outline *() const { return handle; }
};

/**
 * The path, one segment per line, in the same spelling the reference dump uses.
 *
 * Whole font units rather than 26.6, because every coordinate in this file is
 * whole and a test that reads `6400` where it means `100` is a test nobody
 * checks. A fractional coordinate would show up as a failure naming the
 * remainder, which is the right way for that to be noticed.
 */
class Path {
public:
  static std::vector<std::string> of(const GFNT_Outline * outline) {
    Path path;
    GFNT_OutlineSink sink{move_to, line_to, quad_to, cubic_to, close};
    GFNT_Error error{};
    EXPECT_EQ(gfnt_outline_decompose(outline, &sink, &path, &error), GFNT_OK)
        << (error.message ? error.message : "");
    return path.lines_;
  }

  /** The same, for a walk expected to fail; returns the result. */
  static GFNT_Result failure(const GFNT_Outline * outline, GFNT_Error * error) {
    Path path;
    GFNT_OutlineSink sink{move_to, line_to, quad_to, cubic_to, close};
    return gfnt_outline_decompose(outline, &sink, &path, error);
  }

  /** A sink with no quad_to, for the "the sink cannot take this" arm. */
  static GFNT_Result without_quads(const GFNT_Outline * outline,
      GFNT_Error * error) {
    Path path;
    GFNT_OutlineSink sink{move_to, line_to, nullptr, nullptr, close};
    return gfnt_outline_decompose(outline, &sink, &path, error);
  }

private:
  static std::string spell(const char * verb,
      const std::vector<GFNT_Point> & points) {
    std::string out = verb;
    for (const GFNT_Point & point : points) {
      out += " " + std::to_string(point.x / GFNT_F26DOT6_ONE) + " "
          + std::to_string(point.y / GFNT_F26DOT6_ONE);
      const int32_t remainder_x = point.x % GFNT_F26DOT6_ONE;
      const int32_t remainder_y = point.y % GFNT_F26DOT6_ONE;
      if (remainder_x || remainder_y) {
        out += "+" + std::to_string(remainder_x) + "/"
            + std::to_string(remainder_y) + "ths";
      }
    }
    return out;
  }

  static GFNT_Result move_to(void * user, GFNT_Point to) {
    static_cast<Path *>(user)->lines_.push_back(spell("move", {to}));
    return GFNT_OK;
  }
  static GFNT_Result line_to(void * user, GFNT_Point to) {
    static_cast<Path *>(user)->lines_.push_back(spell("line", {to}));
    return GFNT_OK;
  }
  static GFNT_Result quad_to(void * user, GFNT_Point control, GFNT_Point to) {
    static_cast<Path *>(user)->lines_.push_back(spell("quad", {control, to}));
    return GFNT_OK;
  }
  static GFNT_Result cubic_to(void * user, GFNT_Point c1, GFNT_Point c2,
      GFNT_Point to) {
    static_cast<Path *>(user)->lines_.push_back(spell("cubic", {c1, c2, to}));
    return GFNT_OK;
  }
  static GFNT_Result close(void * user) {
    static_cast<Path *>(user)->lines_.push_back("close");
    return GFNT_OK;
  }

  std::vector<std::string> lines_;
};

/** A box, spelled in whole font units. */
std::string box_of(const GFNT_Outline * outline) {
  GFNT_Box box{};
  EXPECT_EQ(gfnt_outline_bounds(outline, &box), GFNT_OK);
  if (gfnt_box_is_empty(&box)) {
    return "empty";
  }
  return std::to_string(box.x_min / GFNT_F26DOT6_ONE) + " "
      + std::to_string(box.y_min / GFNT_F26DOT6_ONE) + " "
      + std::to_string(box.x_max / GFNT_F26DOT6_ONE) + " "
      + std::to_string(box.y_max / GFNT_F26DOT6_ONE);
}

const std::vector<Pt> kLeaf = {
    {100, 0, GFNT_POINT_ON},
    {100, 400, GFNT_POINT_QUAD},
    {400, 400, GFNT_POINT_ON},
    {400, 0, GFNT_POINT_QUAD},
};

TEST(Outline, ANewOutlineHasNothingInItAndIsInFontUnits) {
  Outline outline;

  EXPECT_EQ(gfnt_outline_point_count(outline), 0u);
  EXPECT_EQ(gfnt_outline_contour_count(outline), 0u);
  EXPECT_EQ(gfnt_outline_space(outline), GFNT_OUTLINE_UNITS);
  EXPECT_EQ(box_of(outline), "empty");
  EXPECT_EQ(Path::of(outline), std::vector<std::string>{});
}

TEST(Outline, APointBeforeAnyContourIsACallerError) {
  Outline outline;
  GFNT_Point point{0, 0};
  GFNT_Error error{};

  EXPECT_EQ(gfnt_outline_add_point(outline, point, GFNT_POINT_ON, &error),
      GFNT_ERR_INVALID);
  EXPECT_NE(error.message, nullptr);
}

TEST(Outline, AnExplicitQuadraticWalksAsTheFontStoresIt) {
  Outline outline;
  outline.contour(kLeaf);

  EXPECT_EQ(Path::of(outline), (std::vector<std::string>{
      "move 100 0",
      "quad 100 400 400 400",
      "quad 400 0 100 0",
      "close",
  }));
  EXPECT_EQ(box_of(outline), "100 0 400 400");
}

TEST(Outline, TwoOffCurvePointsInARowImplyTheOnCurvePointBetweenThem) {
  Outline outline;
  outline.contour({
      {100, 0, GFNT_POINT_ON},
      {200, 500, GFNT_POINT_QUAD},
      {400, 500, GFNT_POINT_QUAD},
      {500, 0, GFNT_POINT_ON},
  });

  // (300,500) is in no font: it is the midpoint glyf leaves out.
  EXPECT_EQ(Path::of(outline), (std::vector<std::string>{
      "move 100 0",
      "quad 200 500 300 500",
      "quad 400 500 500 0",
      "close",
  }));
}

TEST(Outline, AContourBeginningOffCurveStartsAtItsLastPoint) {
  Outline outline;
  outline.contour({
      {100, 400, GFNT_POINT_QUAD},
      {400, 400, GFNT_POINT_QUAD},
      {400, 0, GFNT_POINT_QUAD},
      {100, 0, GFNT_POINT_ON},
  });

  EXPECT_EQ(Path::of(outline), (std::vector<std::string>{
      "move 100 0",
      "quad 100 400 250 400",
      "quad 400 400 400 200",
      "quad 400 0 100 0",
      "close",
  }));
  EXPECT_EQ(box_of(outline), "100 0 400 400");
}

TEST(Outline, AContourWithNothingOnCurveStartsAtAnImpliedMidpoint) {
  Outline outline;
  outline.contour({
      {100, 0, GFNT_POINT_QUAD},
      {100, 400, GFNT_POINT_QUAD},
      {400, 400, GFNT_POINT_QUAD},
      {400, 0, GFNT_POINT_QUAD},
  });

  // Every on-curve point is implied, the first of them included: the walk
  // begins midway between the contour's last point and its first.
  EXPECT_EQ(Path::of(outline), (std::vector<std::string>{
      "move 250 0",
      "quad 100 0 100 200",
      "quad 100 400 250 400",
      "quad 400 400 400 200",
      "quad 400 0 250 0",
      "close",
  }));
  EXPECT_EQ(box_of(outline), "100 0 400 400");
}

TEST(Outline, ASameContourStoredFromAnotherPointOfItsCycleDrawsTheSameRegion) {
  // Two spellings of one contour: the second begins off-curve and has two
  // on-curve points, so where the walk *starts* is a choice. The segments are
  // the same cycle either way, which is what a closed contour means, and the
  // box - which has no starting point - is identical.
  Outline first;
  first.contour({
      {100, 0, GFNT_POINT_ON},
      {200, 500, GFNT_POINT_QUAD},
      {400, 500, GFNT_POINT_QUAD},
      {500, 0, GFNT_POINT_ON},
  });
  Outline rotated;
  rotated.contour({
      {200, 500, GFNT_POINT_QUAD},
      {400, 500, GFNT_POINT_QUAD},
      {500, 0, GFNT_POINT_ON},
      {100, 0, GFNT_POINT_ON},
  });

  EXPECT_EQ(box_of(first), box_of(rotated));
  EXPECT_EQ(box_of(first), "100 0 500 500");
  // This library starts at the contour's last point, which is FreeType's rule.
  // fontTools rotates to the first on-curve point and draws the closing line
  // explicitly instead; both are the same cycle, and `tools/oracle/glyf_diff.py`
  // is where that is reconciled.
  EXPECT_EQ(Path::of(rotated), (std::vector<std::string>{
      "move 100 0",
      "quad 200 500 300 500",
      "quad 400 500 500 0",
      "close",
  }));
}

TEST(Outline, AOnePointContourIsAMoveAndACloseAndNoSegment) {
  Outline outline;
  outline.contour({{300, 300, GFNT_POINT_ON}});

  EXPECT_EQ(Path::of(outline), (std::vector<std::string>{
      "move 300 300",
      "close",
  }));
  EXPECT_EQ(box_of(outline), "300 300 300 300");
}

TEST(Outline, AContourWithNoPointsDrawsNothingRatherThanAnEmptyMove) {
  Outline outline;
  GFNT_Error error{};

  ASSERT_EQ(gfnt_outline_begin_contour(outline, &error), GFNT_OK);
  EXPECT_EQ(gfnt_outline_contour_count(outline), 1u);
  EXPECT_EQ(Path::of(outline), std::vector<std::string>{});
}

TEST(Outline, TwoContoursAreWalkedInOrderAndReportedSeparately) {
  Outline outline;
  outline.contour({
      {0, 0, GFNT_POINT_ON}, {100, 0, GFNT_POINT_ON}, {100, 100, GFNT_POINT_ON},
  });
  outline.contour({
      {200, 200, GFNT_POINT_ON}, {300, 200, GFNT_POINT_ON},
  });

  EXPECT_EQ(gfnt_outline_contour_count(outline), 2u);
  EXPECT_EQ(gfnt_outline_point_count(outline), 5u);
  size_t first = 99;
  size_t count = 99;
  ASSERT_EQ(gfnt_outline_contour_at(outline, 0, &first, &count), GFNT_OK);
  EXPECT_EQ(first, 0u);
  EXPECT_EQ(count, 3u);
  ASSERT_EQ(gfnt_outline_contour_at(outline, 1, &first, &count), GFNT_OK);
  EXPECT_EQ(first, 3u);
  EXPECT_EQ(count, 2u);
  EXPECT_EQ(gfnt_outline_contour_at(outline, 2, &first, &count),
      GFNT_ERR_INVALID);

  EXPECT_EQ(Path::of(outline), (std::vector<std::string>{
      "move 0 0", "line 100 0", "line 100 100", "close",
      "move 200 200", "line 300 200", "close",
  }));
}

TEST(Outline, ACubicWalksAsOneSegmentWithTwoControls) {
  Outline outline;
  outline.contour({
      {0, 0, GFNT_POINT_ON},
      {0, 100, GFNT_POINT_CUBIC},
      {100, 100, GFNT_POINT_CUBIC},
      {100, 0, GFNT_POINT_ON},
  });

  EXPECT_EQ(Path::of(outline), (std::vector<std::string>{
      "move 0 0",
      "cubic 0 100 100 100 100 0",
      "close",
  }));
}

TEST(Outline, ACubicBoundIsTighterThanItsControlPoints) {
  Outline outline;
  outline.contour({
      {0, 0, GFNT_POINT_ON},
      {0, 100, GFNT_POINT_CUBIC},
      {100, 100, GFNT_POINT_CUBIC},
      {100, 0, GFNT_POINT_ON},
  });
  GFNT_Box control{};

  ASSERT_EQ(gfnt_outline_control_box(outline, &control), GFNT_OK);
  EXPECT_EQ(control.y_max / GFNT_F26DOT6_ONE, 100);
  // The curve reaches three quarters of the way to its controls and no
  // further, so the tight box is 75 units tall where the control box is 100.
  EXPECT_EQ(box_of(outline), "0 0 100 75");
}

TEST(Outline, ALoneCubicControlPointIsNotASegment) {
  Outline outline;
  GFNT_Error error{};
  outline.contour({
      {0, 0, GFNT_POINT_ON},
      {0, 100, GFNT_POINT_CUBIC},
      {100, 0, GFNT_POINT_ON},
  });

  EXPECT_EQ(Path::failure(outline, &error), GFNT_ERR_INVALID);
  EXPECT_NE(error.message, nullptr);
}

TEST(Outline, AQuadraticAndACubicControlPointCannotBeAdjacent) {
  Outline outline;
  GFNT_Error error{};
  outline.contour({
      {0, 0, GFNT_POINT_ON},
      {0, 100, GFNT_POINT_QUAD},
      {50, 100, GFNT_POINT_CUBIC},
      {100, 100, GFNT_POINT_CUBIC},
      {100, 0, GFNT_POINT_ON},
  });

  EXPECT_EQ(Path::failure(outline, &error), GFNT_ERR_INVALID);
}

TEST(Outline, ThreeCubicControlPointsInARowAreNoCurve) {
  Outline outline;
  GFNT_Error error{};
  outline.contour({
      {0, 0, GFNT_POINT_ON},
      {0, 100, GFNT_POINT_CUBIC},
      {50, 100, GFNT_POINT_CUBIC},
      {100, 100, GFNT_POINT_CUBIC},
      {100, 0, GFNT_POINT_ON},
  });

  EXPECT_EQ(Path::failure(outline, &error), GFNT_ERR_INVALID);
}

TEST(Outline, ASinkWithoutAQuadToIsToldRatherThanLosingTheCurve) {
  Outline outline;
  GFNT_Error error{};
  outline.contour(kLeaf);

  EXPECT_EQ(Path::without_quads(outline, &error), GFNT_ERR_UNSUPPORTED);
  EXPECT_NE(error.message, nullptr);
}

TEST(Outline, ASinkThatFailsStopsTheWalkWithItsOwnResult) {
  struct Counter {
    int moves = 0;
  };
  Outline outline;
  outline.contour(kLeaf);
  outline.contour(kLeaf);
  Counter counter;
  GFNT_OutlineSink sink{};
  sink.move_to = [](void * user, GFNT_Point) -> GFNT_Result {
    Counter * seen = static_cast<Counter *>(user);
    seen->moves += 1;
    return seen->moves > 1 ? GFNT_ERR_OOM : GFNT_OK;
  };
  sink.line_to = [](void *, GFNT_Point) { return GFNT_OK; };
  sink.quad_to = [](void *, GFNT_Point, GFNT_Point) { return GFNT_OK; };
  sink.close = [](void *) { return GFNT_OK; };

  EXPECT_EQ(gfnt_outline_decompose(outline, &sink, &counter, nullptr),
      GFNT_ERR_OOM);
  EXPECT_EQ(counter.moves, 2);
}

TEST(Outline, TranslateMovesEveryPointAndLeavesTheSpaceAlone) {
  Outline outline;
  outline.contour(kLeaf);

  ASSERT_EQ(gfnt_outline_translate(outline, u(10), u(-20)), GFNT_OK);
  EXPECT_EQ(box_of(outline), "110 -20 410 380");
  EXPECT_EQ(gfnt_outline_space(outline), GFNT_OUTLINE_UNITS);
}

TEST(Outline, TransformIsRowMajor) {
  Outline outline;
  outline.contour({{100, 0, GFNT_POINT_ON}, {0, 100, GFNT_POINT_ON}});
  // x' = x + y/4, y' = y/2. A column-major reading would put (100,0) at
  // (100, 25) rather than (100, 0), which is the transposition that shears a
  // composite the wrong way.
  ASSERT_EQ(gfnt_outline_transform(outline, GFNT_F16DOT16_ONE,
      GFNT_F16DOT16_ONE / 4, 0, GFNT_F16DOT16_ONE / 2, 0, 0), GFNT_OK);

  GFNT_Point point{};
  ASSERT_EQ(gfnt_outline_point_at(outline, 0, &point, nullptr), GFNT_OK);
  EXPECT_EQ(point.x, u(100));
  EXPECT_EQ(point.y, u(0));
  ASSERT_EQ(gfnt_outline_point_at(outline, 1, &point, nullptr), GFNT_OK);
  EXPECT_EQ(point.x, u(25));
  EXPECT_EQ(point.y, u(50));
}

TEST(Outline, ScaleTurnsUnitsIntoPixelsOnceAndSaysSo) {
  Outline outline;
  GFNT_Error error{};
  outline.contour(kLeaf);
  // 1000 units per em at 16 pixels: 400 units is 6.4 pixels, which is 409.6
  // in 26.6 and so rounds - a coordinate that is not whole is the point.
  const GFNT_F16Dot16 scale = gfnt_scale_for_ppem(1000, 16);

  ASSERT_EQ(gfnt_outline_scale(outline, scale, &error), GFNT_OK);
  EXPECT_EQ(gfnt_outline_space(outline), GFNT_OUTLINE_PIXELS);
  GFNT_Point point{};
  ASSERT_EQ(gfnt_outline_point_at(outline, 1, &point, nullptr), GFNT_OK);
  EXPECT_EQ(point.x, 102);  // 100 units at 16/1000 is 1.6 pixels: 102.4ths.
  EXPECT_EQ(point.y, 410);  // 400 units is 6.4 pixels: 409.6ths, rounded up.

  // Twice would scale the pixels again, so it is refused rather than done.
  EXPECT_EQ(gfnt_outline_scale(outline, scale, &error), GFNT_ERR_INVALID);
  EXPECT_NE(error.message, nullptr);
}

TEST(Outline, ClearKeepsTheOutlineUsableAndForgetsThePixels) {
  Outline outline;
  GFNT_Error error{};
  outline.contour(kLeaf);
  ASSERT_EQ(gfnt_outline_scale(outline, gfnt_scale_for_ppem(1000, 16), &error),
      GFNT_OK);

  gfnt_outline_clear(outline);
  EXPECT_EQ(gfnt_outline_point_count(outline), 0u);
  EXPECT_EQ(gfnt_outline_contour_count(outline), 0u);
  EXPECT_EQ(gfnt_outline_space(outline), GFNT_OUTLINE_UNITS);
  outline.contour(kLeaf);
  EXPECT_EQ(box_of(outline), "100 0 400 400");
}

TEST(Outline, TheControlBoxContainsTheBoundsAndIsCheaperNotSmaller) {
  Outline outline;
  GFNT_Box bounds{};
  GFNT_Box control{};
  outline.contour(kLeaf);

  ASSERT_EQ(gfnt_outline_bounds(outline, &bounds), GFNT_OK);
  ASSERT_EQ(gfnt_outline_control_box(outline, &control), GFNT_OK);
  EXPECT_LE(control.x_min, bounds.x_min);
  EXPECT_LE(control.y_min, bounds.y_min);
  EXPECT_GE(control.x_max, bounds.x_max);
  EXPECT_GE(control.y_max, bounds.y_max);
}

TEST(Outline, AQuadraticExtremumInsideTheSegmentWidensTheBox) {
  Outline outline;
  // A single quadratic arch: the curve peaks at 50 units, halfway to its
  // control point at 100, so a box taken from the points alone is twice as
  // tall as the glyph.
  outline.contour({
      {0, 0, GFNT_POINT_ON},
      {50, 100, GFNT_POINT_QUAD},
      {100, 0, GFNT_POINT_ON},
  });
  GFNT_Box control{};

  ASSERT_EQ(gfnt_outline_control_box(outline, &control), GFNT_OK);
  EXPECT_EQ(control.y_max / GFNT_F26DOT6_ONE, 100);
  EXPECT_EQ(box_of(outline), "0 0 100 50");
}

TEST(Outline, AQuadraticExtremumBelowTheBaselineIsFlooredNotTruncated) {
  Outline outline;
  outline.contour({
      {0, 0, GFNT_POINT_ON},
      {50, -100, GFNT_POINT_QUAD},
      {100, 0, GFNT_POINT_ON},
  });

  EXPECT_EQ(box_of(outline), "0 -50 100 0");
}

TEST(Outline, ManyContoursAndPointsGrowWithoutTrippingTheDefaultCaps) {
  Outline outline;
  GFNT_Error error{};

  // The caps themselves are a face's to set, so `max_outline_points` is
  // exercised against a font in test_glyf.cpp. What is checked here is the
  // growth: 64 contours of one point each crosses both arrays' first doubling,
  // which is where a parallel-array bug puts a point in one array and its tag
  // nowhere.
  for (int i = 0; i < 64; ++i) {
    GFNT_Point point{u(i), u(i)};
    ASSERT_EQ(gfnt_outline_begin_contour(outline, &error), GFNT_OK);
    ASSERT_EQ(gfnt_outline_add_point(outline, point, GFNT_POINT_ON, &error),
        GFNT_OK) << error.message;
  }
  EXPECT_EQ(gfnt_outline_point_count(outline), 64u);
  EXPECT_EQ(gfnt_outline_contour_count(outline), 64u);
}

TEST(Outline, AnAllocationFailureIsReportedAndNothingLeaks) {
  for (size_t fail_at = 0; fail_at < 6; ++fail_at) {
    gfnttest::FailingAllocator allocator(fail_at, 0);
    GFNT_Outline * outline = nullptr;
    GFNT_Error error{};

    if (gfnt_outline_create(allocator.get(), &outline, &error) != GFNT_OK) {
      EXPECT_EQ(error.result, GFNT_ERR_OOM);
      continue;
    }
    GFNT_Result result = gfnt_outline_begin_contour(outline, &error);
    for (int i = 0; result == GFNT_OK && i < 200; ++i) {
      GFNT_Point point{u(i), 0};
      result = gfnt_outline_add_point(outline, point, GFNT_POINT_ON, &error);
    }
    if (result != GFNT_OK) {
      EXPECT_EQ(result, GFNT_ERR_OOM) << "at request " << fail_at;
    }
    gfnt_outline_destroy(outline);
  }
}

TEST(Outline, EveryTagAndSpaceHasAName) {
  for (int tag = 0; tag < GFNT_POINT_TAG_COUNT; ++tag) {
    EXPECT_STRNE(gfnt_point_tag_string(static_cast<GFNT_PointTag>(tag)),
        "unknown tag");
  }
  EXPECT_STREQ(gfnt_point_tag_string(
      static_cast<GFNT_PointTag>(GFNT_POINT_TAG_COUNT)), "unknown tag");
  for (int space = 0; space < GFNT_OUTLINE_SPACE_COUNT; ++space) {
    EXPECT_STRNE(gfnt_outline_space_string(
        static_cast<GFNT_OutlineSpace>(space)), "unknown space");
  }
  EXPECT_STREQ(gfnt_outline_space_string(
      static_cast<GFNT_OutlineSpace>(GFNT_OUTLINE_SPACE_COUNT)),
      "unknown space");
}

TEST(Outline, NullIsACallerErrorEverywhereAndCrashesNowhere) {
  GFNT_Box box{};
  GFNT_Point point{};
  GFNT_OutlineSink sink{};

  EXPECT_EQ(gfnt_outline_create(nullptr, nullptr, nullptr), GFNT_ERR_INVALID);
  gfnt_outline_destroy(nullptr);
  gfnt_outline_clear(nullptr);
  EXPECT_EQ(gfnt_outline_point_count(nullptr), 0u);
  EXPECT_EQ(gfnt_outline_contour_count(nullptr), 0u);
  EXPECT_EQ(gfnt_outline_space(nullptr), GFNT_OUTLINE_UNITS);
  EXPECT_EQ(gfnt_outline_point_at(nullptr, 0, &point, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_outline_contour_at(nullptr, 0, nullptr, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_outline_add_point(nullptr, point, GFNT_POINT_ON, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_outline_begin_contour(nullptr, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_outline_bounds(nullptr, &box), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_outline_control_box(nullptr, &box), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_outline_transform(nullptr, 0, 0, 0, 0, 0, 0),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_outline_translate(nullptr, 0, 0), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_outline_scale(nullptr, 0, nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_outline_decompose(nullptr, &sink, nullptr, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_outline_dump(nullptr, stdout), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_outline_path_dump(nullptr, stdout), GFNT_ERR_INVALID);
  EXPECT_TRUE(gfnt_box_is_empty(nullptr));
}

TEST(Outline, AnOutlineWithNoPointsDumpsASentenceRatherThanNothing) {
  Outline outline;
  gfnttest::CapturedOutput out;

  ASSERT_NE(out.get(), nullptr);
  EXPECT_EQ(gfnt_outline_dump(outline, out.get()), GFNT_OK);
  EXPECT_EQ(gfnt_outline_path_dump(outline, out.get()), GFNT_OK);
  const std::string text = out.finish();
  EXPECT_NE(text.find("outline: no contours"), std::string::npos) << text;
  EXPECT_NE(text.find("path: empty"), std::string::npos) << text;
}

TEST(Outline, TheDumpNamesEveryPointAndTheBounds) {
  Outline outline;
  gfnttest::CapturedOutput out;
  outline.contour(kLeaf);

  ASSERT_NE(out.get(), nullptr);
  EXPECT_EQ(gfnt_outline_dump(outline, out.get()), GFNT_OK);
  const std::string text = out.finish();
  EXPECT_NE(text.find("outline: space font units, points 4, contours 1"),
      std::string::npos) << text;
  EXPECT_NE(text.find("outline point 1: 6400 25600 quad"), std::string::npos)
      << text;
  EXPECT_NE(text.find("outline bounds: 6400 0 25600 25600"), std::string::npos)
      << text;
}

}  // namespace

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
