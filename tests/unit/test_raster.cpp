/**
 * @file
 *
 * The scan converter, checked against arithmetic that is not its own.
 *
 * **The problem this file exists to solve.** A rasteriser cannot be tested
 * against itself: "render it and see that it is what it rendered" is the shape
 * of a test that passes forever. And fontTools does not rasterise, so the
 * oracle that covers `glyf` has nothing to say here. So this file carries a
 * second implementation of the *definition* of coverage - the fraction of a
 * pixel a path encloses, measured by sampling a grid and counting winding
 * numbers - and requires the two to agree. The two share no code and no
 * approach: one computes area exactly from cell crossings, the other counts
 * points inside. Where they disagree, the sampler's own resolution bounds how
 * much they may.
 *
 * `make check-golden` is the third thing, and answers a different question: not
 * "is this coverage right" but "is it the same number on a big-endian machine".
 *
 * documentation/design.md sections 8.1 and 14.4.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstdlib>
#include <string>
#include <vector>

#include <ghoti.io/font/outline.h>
#include <ghoti.io/font/raster.h>

namespace {

/** 26.6 for a whole number. */
constexpr GFNT_F26Dot6 u(int32_t whole) {
  return static_cast<GFNT_F26Dot6>(whole * GFNT_F26DOT6_ONE);
}

/** One point of a contour under construction, in 26.6. */
struct Pt {
  GFNT_F26Dot6 x;
  GFNT_F26Dot6 y;
  GFNT_PointTag tag = GFNT_POINT_ON;
};

/**
 * An outline built in **pixels**, which is what the rasteriser takes.
 *
 * `gfnt_outline_scale()` is the only thing that sets the pixel space, so a
 * hand-built outline has to go through it: these are built in font units with
 * an em of 64, which scales to pixels one for one, so the numbers written here
 * are the pixel coordinates.
 */
struct Shape {
  GFNT_Outline * handle = nullptr;
  GFNT_Error error{};

  Shape() { EXPECT_EQ(gfnt_outline_create(nullptr, &handle, &error), GFNT_OK); }
  ~Shape() { gfnt_outline_destroy(handle); }
  Shape(const Shape &) = delete;
  Shape & operator=(const Shape &) = delete;

  void contour(const std::vector<Pt> & points) {
    ASSERT_EQ(gfnt_outline_begin_contour(handle, &error), GFNT_OK);
    for (const Pt & point : points) {
      GFNT_Point where{point.x, point.y};
      ASSERT_EQ(gfnt_outline_add_point(handle, where, point.tag, &error),
          GFNT_OK) << error.message;
    }
  }

  /** Declare the outline to be in pixels, one font unit to one 26.6 unit. */
  void ready() {
    ASSERT_EQ(gfnt_outline_scale(handle, GFNT_F16DOT16_ONE, &error), GFNT_OK);
  }

  operator GFNT_Outline *() const { return handle; }
};

/** A coverage owned for the length of a test. */
struct Render {
  GFNT_Coverage coverage{};
  GFNT_Error error{};
  GFNT_Result result = GFNT_ERR_INTERNAL;

  Render(const GFNT_Outline * outline, const GFNT_RasterOptions * options) {
    result = gfnt_raster_outline(outline, options, nullptr, nullptr, &coverage,
        &error);
  }
  ~Render() { gfnt_coverage_destroy(&coverage); }
  Render(const Render &) = delete;
  Render & operator=(const Render &) = delete;
};

/**
 * The reference: coverage by sampling, from the definition.
 *
 * The path is walked into line segments, then each pixel is divided into a
 * `kSamples` x `kSamples` grid and each sample's winding number is counted by
 * the crossing rule. Nothing here knows about cells, areas or sweeps.
 *
 * 16 x 16 = 256 samples per pixel is the resolution, so the sampler's own
 * quantisation is one part in 256 - the same as the output's - and its error on
 * a pixel crossed by one edge is bounded by the samples that edge passes
 * between, which is at most one row or column of them.
 */
class Sampler {
public:
  static constexpr int kSamples = 16;

  explicit Sampler(const GFNT_Outline * outline) {
    GFNT_OutlineSink sink{move_to, line_to, quad_to, cubic_to, close};
    GFNT_Error error{};
    EXPECT_EQ(gfnt_outline_decompose(outline, &sink, this, &error), GFNT_OK);
  }

  /** The coverage of the pixel whose bottom-left corner is (px, py). */
  int coverage(int px, int py, GFNT_FillRule fill) const {
    int inside = 0;
    for (int sy = 0; sy < kSamples; ++sy) {
      for (int sx = 0; sx < kSamples; ++sx) {
        // Sample at the centre of each sub-cell, in 26.6: a sample exactly on
        // an edge would be a coin toss, and centres never are for the integer
        // and half-integer geometry here.
        const GFNT_F26Dot6 x = u(px)
            + static_cast<GFNT_F26Dot6>((2 * sx + 1) * GFNT_F26DOT6_ONE
                / (2 * kSamples));
        const GFNT_F26Dot6 y = u(py)
            + static_cast<GFNT_F26Dot6>((2 * sy + 1) * GFNT_F26DOT6_ONE
                / (2 * kSamples));
        const int winding = wind(x, y);
        if (fill == GFNT_FILL_EVEN_ODD ? (winding & 1) != 0 : winding != 0) {
          inside += 1;
        }
      }
    }
    // 256 samples to 0..255: a fully covered pixel is 255, not 256.
    return inside == kSamples * kSamples ? 255
                                         : inside * 255 / (kSamples * kSamples);
  }

private:
  struct Edge {
    GFNT_Point from;
    GFNT_Point to;
  };

  /** The winding number of a point, by counting signed crossings to the right. */
  int wind(GFNT_F26Dot6 x, GFNT_F26Dot6 y) const {
    int winding = 0;
    for (const Edge & edge : edges_) {
      const GFNT_F26Dot6 y0 = edge.from.y;
      const GFNT_F26Dot6 y1 = edge.to.y;
      if ((y0 <= y) == (y1 <= y)) {
        continue;
      }
      // Where the edge crosses this scanline, in 64ths of a 26.6 unit, so the
      // comparison needs no division and no rounding.
      const int64_t dy = static_cast<int64_t>(y1) - y0;
      const int64_t dx = static_cast<int64_t>(edge.to.x) - edge.from.x;
      const int64_t at = static_cast<int64_t>(edge.from.x) * dy
          + dx * (static_cast<int64_t>(y) - y0);
      const int64_t here = static_cast<int64_t>(x) * dy;
      const bool crossing_right = dy > 0 ? at > here : at < here;
      if (crossing_right) {
        winding += dy > 0 ? 1 : -1;
      }
    }
    return winding;
  }

  void line(GFNT_Point to) {
    edges_.push_back({current_, to});
    current_ = to;
  }

  static GFNT_Point mid(GFNT_Point a, GFNT_Point b) {
    GFNT_Point out;
    out.x = static_cast<GFNT_F26Dot6>((static_cast<int64_t>(a.x) + b.x) / 2);
    out.y = static_cast<GFNT_F26Dot6>((static_cast<int64_t>(a.y) + b.y) / 2);
    return out;
  }

  /** Flatten far finer than the rasteriser does, so the curve is not the error. */
  void flatten_quad(GFNT_Point p0, GFNT_Point p1, GFNT_Point p2, int depth) {
    if (depth == 0) {
      line(p2);
      return;
    }
    const GFNT_Point a = mid(p0, p1);
    const GFNT_Point b = mid(p1, p2);
    const GFNT_Point m = mid(a, b);
    flatten_quad(p0, a, m, depth - 1);
    flatten_quad(m, b, p2, depth - 1);
  }

  void flatten_cubic(GFNT_Point p0, GFNT_Point p1, GFNT_Point p2,
      GFNT_Point p3, int depth) {
    if (depth == 0) {
      line(p3);
      return;
    }
    const GFNT_Point a = mid(p0, p1);
    const GFNT_Point b = mid(p1, p2);
    const GFNT_Point c = mid(p2, p3);
    const GFNT_Point d = mid(a, b);
    const GFNT_Point e = mid(b, c);
    const GFNT_Point m = mid(d, e);
    flatten_cubic(p0, a, d, m, depth - 1);
    flatten_cubic(m, e, c, p3, depth - 1);
  }

  static GFNT_Result move_to(void * user, GFNT_Point to) {
    Sampler * self = static_cast<Sampler *>(user);
    self->current_ = to;
    self->start_ = to;
    return GFNT_OK;
  }
  static GFNT_Result line_to(void * user, GFNT_Point to) {
    static_cast<Sampler *>(user)->line(to);
    return GFNT_OK;
  }
  static GFNT_Result quad_to(void * user, GFNT_Point c, GFNT_Point to) {
    Sampler * self = static_cast<Sampler *>(user);
    self->flatten_quad(self->current_, c, to, 8);
    return GFNT_OK;
  }
  static GFNT_Result cubic_to(void * user, GFNT_Point c1, GFNT_Point c2,
      GFNT_Point to) {
    Sampler * self = static_cast<Sampler *>(user);
    self->flatten_cubic(self->current_, c1, c2, to, 8);
    return GFNT_OK;
  }
  static GFNT_Result close(void * user) {
    Sampler * self = static_cast<Sampler *>(user);
    self->line(self->start_);
    return GFNT_OK;
  }

  std::vector<Edge> edges_;
  GFNT_Point current_{0, 0};
  GFNT_Point start_{0, 0};
};

/**
 * Compare a rendering against the sampler, and say where and by how much.
 *
 * Returns the largest difference found. The caller asserts a bound on it, so a
 * failure names a number rather than "they differ".
 */
int worst_difference(const GFNT_Coverage & coverage, const Sampler & sampler,
    GFNT_FillRule fill, std::string * where) {
  int worst = 0;
  for (uint32_t row = 0; row < coverage.height; ++row) {
    for (uint32_t column = 0; column < coverage.width; ++column) {
      const int mine = gfnt_coverage_at(&coverage, column, row);
      // Row 0 is the top, and `top` is its upper edge, so the pixel's lower
      // edge in glyph space is top - row - 1.
      const int theirs = sampler.coverage(coverage.left + (int)column,
          coverage.top - (int)row - 1, fill);
      const int difference = std::abs(mine - theirs);
      if (difference > worst) {
        worst = difference;
        if (where) {
          *where = "at column " + std::to_string(column) + ", row "
              + std::to_string(row) + ": rasteriser " + std::to_string(mine)
              + ", sampler " + std::to_string(theirs);
        }
      }
    }
  }
  return worst;
}

/** Every pixel the sampler says is covered but the rendering has no room for. */
int missed_outside(const GFNT_Coverage & coverage, const Sampler & sampler,
    GFNT_FillRule fill, int reach) {
  int worst = 0;
  for (int y = coverage.top - (int)coverage.height - reach;
      y < coverage.top + reach; ++y) {
    for (int x = coverage.left - reach;
        x < coverage.left + (int)coverage.width + reach; ++x) {
      const bool inside = x >= coverage.left
          && x < coverage.left + (int)coverage.width
          && y < coverage.top && y >= coverage.top - (int)coverage.height;
      if (inside) {
        continue;
      }
      const int theirs = sampler.coverage(x, y, fill);
      if (theirs > worst) {
        worst = theirs;
      }
    }
  }
  return worst;
}

/**
 * The trimmed bitmap covers exactly the outline's own bounding box.
 *
 * `gfnt_outline_bounds()` solves the curves; the rasteriser flattens them. So
 * this is a second opinion about the extent, and it is the assertion an empty
 * rendering cannot satisfy - which matters, because `worst_difference()` over a
 * bitmap with no pixels compares nothing and reports perfect agreement. A
 * planted defect that flattened a cubic to its chord was invisible until this
 * existed.
 *
 * It holds where a pixel the shape touches at all gets a coverage of at least
 * one: a shape whose apex enters a pixel by less than a five-hundredth of it
 * rounds to zero and is trimmed away, so the shapes that use this are the ones
 * whose extremes are on or near pixel boundaries.
 */
void expect_covers_bounds(const GFNT_Outline * outline,
    const GFNT_Coverage & coverage) {
  GFNT_Box box{};
  ASSERT_EQ(gfnt_outline_bounds(outline, &box), GFNT_OK);
  const int32_t left = gfnt_f26dot6_floor(box.x_min);
  const int32_t right = gfnt_f26dot6_ceil(box.x_max);
  const int32_t bottom = gfnt_f26dot6_floor(box.y_min);
  const int32_t top = gfnt_f26dot6_ceil(box.y_max);
  EXPECT_EQ(coverage.left, left);
  EXPECT_EQ(coverage.top, top);
  EXPECT_EQ(coverage.width, static_cast<uint32_t>(right - left));
  EXPECT_EQ(coverage.height, static_cast<uint32_t>(top - bottom));
}

TEST(Raster, AWholePixelRectangleIsFullyCovered) {
  Shape shape;
  shape.contour({{u(0), u(0)}, {u(2), u(0)}, {u(2), u(3)}, {u(0), u(3)}});
  shape.ready();
  Render render(shape, nullptr);

  ASSERT_EQ(render.result, GFNT_OK) << render.error.message;
  EXPECT_EQ(render.coverage.width, 2u);
  EXPECT_EQ(render.coverage.height, 3u);
  EXPECT_EQ(render.coverage.left, 0);
  EXPECT_EQ(render.coverage.top, 3);
  EXPECT_EQ(gfnt_coverage_total(&render.coverage), 6u * 255u);
  for (uint32_t row = 0; row < 3; ++row) {
    for (uint32_t column = 0; column < 2; ++column) {
      EXPECT_EQ(gfnt_coverage_at(&render.coverage, column, row), 255)
          << column << "," << row;
    }
  }
}

TEST(Raster, TheWindingDirectionDoesNotChangeTheCoverage) {
  // Coverage is unsigned: a contour wound the other way encloses the same
  // region. A rasteriser that took the sign of the accumulated area would
  // render one of these and not the other.
  Shape clockwise;
  clockwise.contour({{u(0), u(0)}, {u(2), u(0)}, {u(2), u(2)}, {u(0), u(2)}});
  clockwise.ready();
  Shape widdershins;
  widdershins.contour({{u(0), u(0)}, {u(0), u(2)}, {u(2), u(2)}, {u(2), u(0)}});
  widdershins.ready();
  Render first(clockwise, nullptr);
  Render second(widdershins, nullptr);

  ASSERT_EQ(first.result, GFNT_OK);
  ASSERT_EQ(second.result, GFNT_OK);
  EXPECT_EQ(gfnt_coverage_hash(&first.coverage),
      gfnt_coverage_hash(&second.coverage));
}

TEST(Raster, AHalfPixelRectangleIsHalfCovered) {
  Shape shape;
  // Half a pixel wide: 32 is half of 64.
  shape.contour({{0, 0}, {32, 0}, {32, u(1)}, {0, u(1)}});
  shape.ready();
  Render render(shape, nullptr);

  ASSERT_EQ(render.result, GFNT_OK) << render.error.message;
  EXPECT_EQ(render.coverage.width, 1u);
  EXPECT_EQ(render.coverage.height, 1u);
  EXPECT_EQ(gfnt_coverage_at(&render.coverage, 0, 0), 128);
}

TEST(Raster, AQuarterPixelSquareIsAQuarterOfAQuarter) {
  Shape shape;
  shape.contour({{0, 0}, {16, 0}, {16, 16}, {0, 16}});
  shape.ready();
  Render render(shape, nullptr);

  ASSERT_EQ(render.result, GFNT_OK) << render.error.message;
  // A sixteenth of a pixel: 255/16 is 15.9, and the rasteriser's own rounding
  // is what decides between 15 and 16.
  const uint8_t value = gfnt_coverage_at(&render.coverage, 0, 0);
  EXPECT_GE(value, 15);
  EXPECT_LE(value, 16);
}

TEST(Raster, ATriangleAgreesWithTheSampler) {
  Shape shape;
  shape.contour({{u(0), u(0)}, {u(8), u(0)}, {u(0), u(6)}});
  shape.ready();
  Render render(shape, nullptr);
  Sampler sampler(shape);
  std::string where;

  ASSERT_EQ(render.result, GFNT_OK) << render.error.message;
  // A diagonal edge is where the two methods can differ at all: the sampler
  // quantises the edge to its 16x16 grid. One sixteenth of a pixel is 16 of
  // 255, and the bound is that.
  EXPECT_LE(worst_difference(render.coverage, sampler, GFNT_FILL_NONZERO,
      &where), 16) << where;
  EXPECT_EQ(missed_outside(render.coverage, sampler, GFNT_FILL_NONZERO, 2), 0);
}

TEST(Raster, ACurveAgreesWithTheSampler) {
  Shape shape;
  shape.contour({
      {u(0), u(0)},
      {u(6), u(10), GFNT_POINT_QUAD},
      {u(12), u(0)},
  });
  shape.ready();
  Render render(shape, nullptr);
  Sampler sampler(shape);
  std::string where;

  ASSERT_EQ(render.result, GFNT_OK) << render.error.message;
  expect_covers_bounds(shape, render.coverage);
  EXPECT_LE(worst_difference(render.coverage, sampler, GFNT_FILL_NONZERO,
      &where), 20) << where;
  EXPECT_EQ(missed_outside(render.coverage, sampler, GFNT_FILL_NONZERO, 2), 0);
}

TEST(Raster, ACubicAgreesWithTheSampler) {
  Shape shape;
  shape.contour({
      {u(0), u(0)},
      {u(0), u(10), GFNT_POINT_CUBIC},
      {u(12), u(10), GFNT_POINT_CUBIC},
      {u(12), u(0)},
  });
  shape.ready();
  Render render(shape, nullptr);
  Sampler sampler(shape);
  std::string where;

  ASSERT_EQ(render.result, GFNT_OK) << render.error.message;
  expect_covers_bounds(shape, render.coverage);
  EXPECT_LE(worst_difference(render.coverage, sampler, GFNT_FILL_NONZERO,
      &where), 20) << where;
  EXPECT_EQ(missed_outside(render.coverage, sampler, GFNT_FILL_NONZERO, 2), 0);
}

TEST(Raster, AHoleIsEmptyUnderNonZeroWindingAndOnlyIfItIsWoundBack) {
  // The outer contour anticlockwise and the inner clockwise: the winding
  // numbers cancel and the middle is empty. This is what every font does for
  // a counter, and a rasteriser that took the absolute value per contour
  // rather than summing the winding would fill it in.
  Shape shape;
  shape.contour({{u(0), u(0)}, {u(6), u(0)}, {u(6), u(6)}, {u(0), u(6)}});
  shape.contour({{u(2), u(2)}, {u(2), u(4)}, {u(4), u(4)}, {u(4), u(2)}});
  shape.ready();
  Render render(shape, nullptr);
  Sampler sampler(shape);
  std::string where;

  ASSERT_EQ(render.result, GFNT_OK) << render.error.message;
  EXPECT_EQ(gfnt_coverage_at(&render.coverage, 2, 2), 0);
  EXPECT_EQ(gfnt_coverage_at(&render.coverage, 3, 3), 0);
  EXPECT_EQ(gfnt_coverage_at(&render.coverage, 0, 0), 255);
  EXPECT_LE(worst_difference(render.coverage, sampler, GFNT_FILL_NONZERO,
      &where), 1) << where;
}

TEST(Raster, TwoContoursWoundTheSameWayOverlapUnderNonZeroAndCancelUnderEvenOdd) {
  // The deciding case between the two fill rules, and the only shape where
  // they differ: both squares wound the same way, so the overlap has winding 2
  // - inside under non-zero, outside under even-odd.
  Shape shape;
  shape.contour({{u(0), u(0)}, {u(4), u(0)}, {u(4), u(4)}, {u(0), u(4)}});
  shape.contour({{u(2), u(2)}, {u(6), u(2)}, {u(6), u(6)}, {u(2), u(6)}});
  shape.ready();
  GFNT_RasterOptions nonzero{};
  GFNT_RasterOptions evenodd{};
  evenodd.fill = GFNT_FILL_EVEN_ODD;
  Render filled(shape, &nonzero);
  Render folded(shape, &evenodd);
  Sampler sampler(shape);
  std::string where;

  ASSERT_EQ(filled.result, GFNT_OK);
  ASSERT_EQ(folded.result, GFNT_OK);
  // The overlap is the square from (2,2) to (4,4). In the rendering it is
  // columns 2..3 and, counting down from a top of 6, rows 2..3.
  EXPECT_EQ(gfnt_coverage_at(&filled.coverage, 2, 2), 255);
  EXPECT_EQ(gfnt_coverage_at(&folded.coverage, 2, 2), 0);
  EXPECT_NE(gfnt_coverage_hash(&filled.coverage),
      gfnt_coverage_hash(&folded.coverage));
  EXPECT_EQ(filled.coverage.fill, GFNT_FILL_NONZERO);
  EXPECT_EQ(folded.coverage.fill, GFNT_FILL_EVEN_ODD);
  EXPECT_LE(worst_difference(filled.coverage, sampler, GFNT_FILL_NONZERO,
      &where), 1) << where;
  EXPECT_LE(worst_difference(folded.coverage, sampler, GFNT_FILL_EVEN_ODD,
      &where), 1) << where;
}

TEST(Raster, TwoSeparateEdgesCrossingOnePixelAreBothCounted) {
  // Two thin bars a quarter of a pixel apart, both inside column 0. Every
  // other shape in this file has at most one edge per pixel, so the cell for
  // this column is the only one that receives contributions from two edges
  // that are not consecutive - which is what the per-row merge is for. Without
  // the merge the sweep writes the pixel twice and the second write, carrying
  // only the second bar's area, wins.
  //
  // The order matters and is the whole design of this shape. Cells are merged
  // twice: once as they are made, when a walk stays in the same cell, and once
  // per row afterwards. Two bars in one column would be merged by the first of
  // those - the second bar's first cell is the same cell the first bar's last
  // one was - and the row merge would have nothing to do. So the middle bar
  // sits in a different column, which breaks the run and leaves two entries for
  // column 0 that only the row merge can combine.
  Shape shape;
  shape.contour({{0, 0}, {16, 0}, {16, u(1)}, {0, u(1)}});
  shape.contour({{u(2), 0}, {u(2) + 16, 0}, {u(2) + 16, u(1)}, {u(2), u(1)}});
  shape.contour({{32, 0}, {48, 0}, {48, u(1)}, {32, u(1)}});
  shape.ready();
  Render render(shape, nullptr);
  Sampler sampler(shape);
  std::string where;

  ASSERT_EQ(render.result, GFNT_OK) << render.error.message;
  EXPECT_EQ(render.coverage.width, 3u);
  EXPECT_EQ(render.coverage.height, 1u);
  // A quarter of the pixel from each of the two bars in column 0: half of it.
  EXPECT_EQ(gfnt_coverage_at(&render.coverage, 0, 0), 128);
  EXPECT_EQ(gfnt_coverage_at(&render.coverage, 1, 0), 0);
  EXPECT_EQ(gfnt_coverage_at(&render.coverage, 2, 0), 64);
  EXPECT_LE(worst_difference(render.coverage, sampler, GFNT_FILL_NONZERO,
      &where), 1) << where;
}

TEST(Raster, ASubPixelOffsetMovesTheCoverageAndKeepsTheArea) {
  // The same square rendered at four sub-pixel offsets. Its area does not
  // change, which is the invariant; where the coverage lands does, which is
  // what sub-pixel positioning is for.
  const GFNT_F26Dot6 offsets[] = {0, 16, 32, 48};
  uint64_t first_total = 0;
  std::vector<uint64_t> hashes;

  for (GFNT_F26Dot6 offset : offsets) {
    Shape shape;
    shape.contour({{u(0), u(0)}, {u(3), u(0)}, {u(3), u(3)}, {u(0), u(3)}});
    shape.ready();
    GFNT_RasterOptions options{};
    options.origin_x = offset;
    Render render(shape, &options);

    ASSERT_EQ(render.result, GFNT_OK) << render.error.message;
    EXPECT_EQ(render.coverage.origin_x, offset);
    if (offset == 0) {
      first_total = gfnt_coverage_total(&render.coverage);
      EXPECT_EQ(render.coverage.width, 3u);
    }
    else {
      // The area is the same to within the rounding of the two edges that are
      // no longer on a pixel boundary.
      const uint64_t total = gfnt_coverage_total(&render.coverage);
      EXPECT_GE(total + 8u, first_total);
      EXPECT_LE(total, first_total + 8u);
      EXPECT_EQ(render.coverage.width, 4u);
    }
    hashes.push_back(gfnt_coverage_hash(&render.coverage));
  }
  for (size_t i = 1; i < hashes.size(); ++i) {
    EXPECT_NE(hashes[0], hashes[i]) << "offset " << offsets[i];
  }
}

TEST(Raster, TranslatingAShapeByWholePixelsOnlyMovesIt) {
  Shape here;
  here.contour({{u(0), u(0)}, {u(5), u(0)}, {u(2), u(4)}});
  here.ready();
  Shape there;
  there.contour({{u(7), u(11)}, {u(12), u(11)}, {u(9), u(15)}});
  there.ready();
  Render first(here, nullptr);
  Render second(there, nullptr);

  ASSERT_EQ(first.result, GFNT_OK);
  ASSERT_EQ(second.result, GFNT_OK);
  EXPECT_EQ(first.coverage.width, second.coverage.width);
  EXPECT_EQ(first.coverage.height, second.coverage.height);
  EXPECT_EQ(gfnt_coverage_total(&first.coverage),
      gfnt_coverage_total(&second.coverage));
  EXPECT_EQ(second.coverage.left - first.coverage.left, 7);
  EXPECT_EQ(second.coverage.top - first.coverage.top, 11);
  // Pixel for pixel, not just in total.
  for (uint32_t row = 0; row < first.coverage.height; ++row) {
    for (uint32_t column = 0; column < first.coverage.width; ++column) {
      EXPECT_EQ(gfnt_coverage_at(&first.coverage, column, row),
          gfnt_coverage_at(&second.coverage, column, row))
          << column << "," << row;
    }
  }
}

TEST(Raster, AShapeWithNoAreaRendersNothingRatherThanFailing) {
  Shape shape;
  // A degenerate contour: three collinear points enclose nothing.
  shape.contour({{u(0), u(0)}, {u(4), u(0)}, {u(8), u(0)}});
  shape.ready();
  Render render(shape, nullptr);

  ASSERT_EQ(render.result, GFNT_OK) << render.error.message;
  EXPECT_EQ(render.coverage.width, 0u);
  EXPECT_EQ(render.coverage.height, 0u);
  EXPECT_EQ(render.coverage.data, nullptr);
  EXPECT_EQ(gfnt_coverage_total(&render.coverage), 0u);
}

TEST(Raster, AnEmptyOutlineRendersAnEmptyCoverage) {
  Shape shape;
  shape.ready();
  Render render(shape, nullptr);

  ASSERT_EQ(render.result, GFNT_OK) << render.error.message;
  EXPECT_EQ(render.coverage.width, 0u);
  EXPECT_EQ(render.coverage.data, nullptr);
}

TEST(Raster, AnOutlineInFontUnitsIsRefusedRatherThanRenderedHuge) {
  Shape shape;
  shape.contour({{u(0), u(0)}, {u(2), u(0)}, {u(2), u(2)}});
  // Deliberately not ready(): still in font units.
  GFNT_Coverage coverage{};
  GFNT_Error error{};

  EXPECT_EQ(gfnt_raster_outline(shape, nullptr, nullptr, nullptr, &coverage,
      &error), GFNT_ERR_INVALID);
  ASSERT_NE(error.message, nullptr);
  EXPECT_NE(std::string(error.message).find("font units"), std::string::npos)
      << error.message;
  gfnt_coverage_destroy(&coverage);
}

TEST(Raster, AFillRuleThatDoesNotExistIsACallerError) {
  Shape shape;
  shape.contour({{u(0), u(0)}, {u(2), u(0)}, {u(2), u(2)}});
  shape.ready();
  GFNT_RasterOptions options{};
  options.fill = static_cast<GFNT_FillRule>(GFNT_FILL_RULE_COUNT);
  GFNT_Coverage coverage{};
  GFNT_Error error{};

  EXPECT_EQ(gfnt_raster_outline(shape, &options, nullptr, nullptr, &coverage,
      &error), GFNT_ERR_INVALID);
  gfnt_coverage_destroy(&coverage);
}

TEST(Raster, ACoverageLargerThanTheCapIsALimit) {
  Shape shape;
  shape.contour({{u(0), u(0)}, {u(40), u(0)}, {u(40), u(40)}, {u(0), u(40)}});
  shape.ready();
  GFNT_Limits limits{};
  GFNT_Coverage coverage{};
  GFNT_Error error{};

  gfnt_limits_default(&limits);
  limits.max_raster_bytes = 100;
  EXPECT_EQ(gfnt_raster_outline(shape, nullptr, &limits, nullptr, &coverage,
      &error), GFNT_ERR_LIMIT);
  EXPECT_NE(error.message, nullptr);
  gfnt_coverage_destroy(&coverage);

  // And the width cap, which is a different number and a different message.
  limits.max_raster_bytes = 1u << 20;
  limits.max_ppem = 8;
  EXPECT_EQ(gfnt_raster_outline(shape, nullptr, &limits, nullptr, &coverage,
      &error), GFNT_ERR_LIMIT);
  gfnt_coverage_destroy(&coverage);
}

TEST(Raster, AnAllocationFailureIsReportedAndNothingLeaks) {
  for (size_t fail_at = 0; fail_at < 10; ++fail_at) {
    gfnttest::FailingAllocator allocator(fail_at, 0);
    GFNT_Outline * outline = nullptr;
    GFNT_Coverage coverage{};
    GFNT_Error error{};

    if (gfnt_outline_create(allocator.get(), &outline, &error) != GFNT_OK) {
      continue;
    }
    GFNT_Result result = gfnt_outline_begin_contour(outline, &error);
    const GFNT_Point points[] = {{u(0), u(0)}, {u(9), u(0)}, {u(4), u(7)}};
    for (const GFNT_Point & point : points) {
      if (result != GFNT_OK) {
        break;
      }
      result = gfnt_outline_add_point(outline, point, GFNT_POINT_ON, &error);
    }
    if (result == GFNT_OK) {
      result = gfnt_outline_scale(outline, GFNT_F16DOT16_ONE, &error);
    }
    if (result == GFNT_OK) {
      result = gfnt_raster_outline(outline, nullptr, nullptr, allocator.get(),
          &coverage, &error);
      if (result != GFNT_OK) {
        EXPECT_EQ(result, GFNT_ERR_OOM) << "at request " << fail_at;
      }
    }
    gfnt_coverage_destroy(&coverage);
    gfnt_outline_destroy(outline);
  }
}

TEST(Raster, TheHashCoversEveryFieldItClaimsTo) {
  Shape shape;
  shape.contour({{u(0), u(0)}, {u(3), u(0)}, {u(3), u(3)}, {u(0), u(3)}});
  shape.ready();
  Render render(shape, nullptr);

  ASSERT_EQ(render.result, GFNT_OK);
  const uint64_t base = gfnt_coverage_hash(&render.coverage);
  EXPECT_NE(base, 0u);
  // Each field in the hash's documented list, moved one at a time. A hash that
  // ignored `left` would report two glyphs at different positions as one
  // rendering, and the golden gate would go green through a shifted glyph.
  GFNT_Coverage copy = render.coverage;
  copy.left += 1;
  EXPECT_NE(gfnt_coverage_hash(&copy), base);
  copy = render.coverage;
  copy.top += 1;
  EXPECT_NE(gfnt_coverage_hash(&copy), base);
  copy = render.coverage;
  copy.origin_x = 16;
  EXPECT_NE(gfnt_coverage_hash(&copy), base);
  copy = render.coverage;
  copy.origin_y = 16;
  EXPECT_NE(gfnt_coverage_hash(&copy), base);
  copy = render.coverage;
  copy.fill = GFNT_FILL_EVEN_ODD;
  EXPECT_NE(gfnt_coverage_hash(&copy), base);
  copy = render.coverage;
  copy.width -= 1;
  EXPECT_NE(gfnt_coverage_hash(&copy), base);
  copy = render.coverage;
  copy.height -= 1;
  EXPECT_NE(gfnt_coverage_hash(&copy), base);
  // And one pixel.
  render.coverage.data[0] ^= 1u;
  EXPECT_NE(gfnt_coverage_hash(&render.coverage), base);
}

TEST(Raster, TheHashIgnoresThePaddingItSaysItIgnores) {
  Shape shape;
  shape.contour({{u(0), u(0)}, {u(3), u(0)}, {u(3), u(2)}, {u(0), u(2)}});
  shape.ready();
  Render render(shape, nullptr);

  ASSERT_EQ(render.result, GFNT_OK);
  ASSERT_EQ(render.coverage.stride, render.coverage.width);
  // A wider stride with the same rows is the same rendering. Built by hand
  // because the rasteriser never produces one, which is exactly why the claim
  // needs checking rather than assuming.
  const uint64_t base = gfnt_coverage_hash(&render.coverage);
  const size_t stride = render.coverage.width + 3u;
  std::vector<uint8_t> padded(stride * render.coverage.height, 0xEE);
  for (uint32_t row = 0; row < render.coverage.height; ++row) {
    for (uint32_t column = 0; column < render.coverage.width; ++column) {
      padded[row * stride + column] =
          gfnt_coverage_at(&render.coverage, column, row);
    }
  }
  GFNT_Coverage copy = render.coverage;
  copy.data = padded.data();
  copy.stride = stride;
  copy.allocator = nullptr;
  EXPECT_EQ(gfnt_coverage_hash(&copy), base);
}

TEST(Raster, ATableIsAppliedToEveryPixel) {
  Shape shape;
  shape.contour({{u(0), u(0)}, {u(2), u(0)}, {u(2), u(2)}, {u(0), u(2)}});
  shape.ready();
  Render render(shape, nullptr);
  uint8_t table[256];

  ASSERT_EQ(render.result, GFNT_OK);
  for (int i = 0; i < 256; ++i) {
    table[i] = static_cast<uint8_t>(255 - i);
  }
  ASSERT_EQ(gfnt_coverage_apply_table(&render.coverage, table), GFNT_OK);
  EXPECT_EQ(gfnt_coverage_at(&render.coverage, 0, 0), 0);
  EXPECT_EQ(gfnt_coverage_apply_table(&render.coverage, nullptr),
      GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_coverage_apply_table(nullptr, table), GFNT_ERR_INVALID);
}

TEST(Raster, EveryFillRuleHasAName) {
  for (int rule = 0; rule < GFNT_FILL_RULE_COUNT; ++rule) {
    EXPECT_STRNE(gfnt_fill_rule_string(static_cast<GFNT_FillRule>(rule)),
        "unknown fill rule");
  }
  EXPECT_STREQ(gfnt_fill_rule_string(
      static_cast<GFNT_FillRule>(GFNT_FILL_RULE_COUNT)), "unknown fill rule");
}

TEST(Raster, NullIsACallerErrorAndCrashesNowhere) {
  GFNT_Coverage coverage{};

  EXPECT_EQ(gfnt_raster_outline(nullptr, nullptr, nullptr, nullptr, &coverage,
      nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_face_render_glyph(nullptr, 0, 16, nullptr, nullptr, &coverage,
      nullptr), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_coverage_at(nullptr, 0, 0), 0);
  EXPECT_EQ(gfnt_coverage_total(nullptr), 0u);
  EXPECT_NE(gfnt_coverage_hash(nullptr), 0u);
  EXPECT_EQ(gfnt_coverage_dump(nullptr, stdout), GFNT_ERR_INVALID);
  EXPECT_EQ(gfnt_coverage_dump_art(nullptr, stdout), GFNT_ERR_INVALID);
  gfnt_coverage_destroy(nullptr);
  // Destroying twice is harmless, which is what leaves the struct zeroed.
  gfnt_coverage_destroy(&coverage);
  gfnt_coverage_destroy(&coverage);
}

TEST(Raster, TheDumpsSayWhatTheyHave) {
  Shape shape;
  shape.contour({{u(0), u(0)}, {u(2), u(0)}, {u(2), u(2)}, {u(0), u(2)}});
  shape.ready();
  Render render(shape, nullptr);
  gfnttest::CapturedOutput out;

  ASSERT_EQ(render.result, GFNT_OK);
  ASSERT_NE(out.get(), nullptr);
  EXPECT_EQ(gfnt_coverage_dump(&render.coverage, out.get()), GFNT_OK);
  EXPECT_EQ(gfnt_coverage_dump_art(&render.coverage, out.get()), GFNT_OK);
  const std::string text = out.finish();
  EXPECT_NE(text.find("coverage: 2x2 at 0,2"), std::string::npos) << text;
  EXPECT_NE(text.find("non-zero"), std::string::npos) << text;
  EXPECT_NE(text.find("@@"), std::string::npos) << text;
}

TEST(Raster, AnEmptyCoverageDrawsASentenceRatherThanNothing) {
  GFNT_Coverage coverage{};
  gfnttest::CapturedOutput out;

  ASSERT_NE(out.get(), nullptr);
  EXPECT_EQ(gfnt_coverage_dump_art(&coverage, out.get()), GFNT_OK);
  EXPECT_NE(out.finish().find("nothing to draw"), std::string::npos);
}

}  // namespace

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
