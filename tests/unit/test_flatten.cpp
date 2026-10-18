/**
 * @file
 *
 * The curve flattener, checked against the curve.
 *
 * `src/raster/flatten.c` states and proves one property: for every curve with
 * int32 control points and every tolerance the rasteriser accepts, the polyline
 * is within that tolerance of the curve. Pixels cannot test that - a coverage
 * hash says "different", never "by how much" - so this file measures it
 * directly: the curve is evaluated exactly at a dense set of parameters inside
 * every piece the flattener made, and the distance to that piece's segment is
 * the number asserted.
 *
 * **The evaluation shares nothing with the flattener.** It is the Bernstein form
 * in `long double` at dyadic parameters, which are exact in the type, so the
 * only error is the arithmetic of the sum: below 2^-33 of a unit where
 * `long double` is the x87 type and below a millionth of a unit where it is a
 * plain `double`, against a margin of 0.12 unit.
 *
 * Why a piece-by-piece measure and not a uniform one: a curve spanning the int32
 * range is 2^33 units long, so no affordable uniform sampling is dense enough to
 * see a deviation of a unit between two samples. Inside a piece, the deviation
 * from the chord is a smooth function of the parameter (a cubic polynomial), so
 * thirty-three samples find its maximum to within a thousandth of it.
 *
 * documentation/design.md section 8.1.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cmath>
#include <cstdint>
#include <vector>

#include <ghoti.io/font/outline.h>
#include <ghoti.io/font/raster.h>

#include "../../src/raster/flatten.h"

namespace {

constexpr int32_t INT32_MIN_ = INT32_MIN;

/** One emitted vertex and the piece of the curve it ends. */
struct Vertex {
  GFNT_Point at;
  uint32_t leaf;
  int level;
};

void collect(void * user, GFNT_Point to, uint32_t leaf, int level) {
  static_cast<std::vector<Vertex> *>(user)->push_back({to, leaf, level});
}

/** A curve of either degree. */
struct Curve {
  bool cubic;
  GFNT_Point p[4];
};

int flatten(const Curve & curve, GFNT_F26Dot6 tolerance,
    std::vector<Vertex> & out) {
  out.clear();
  return curve.cubic
      ? gfnt_flatten_cubic(tolerance, curve.p[0], curve.p[1], curve.p[2],
            curve.p[3], collect, &out)
      : gfnt_flatten_quad(tolerance, curve.p[0], curve.p[1], curve.p[2], collect,
            &out);
}

struct Vec {
  long double x;
  long double y;
};

/** The curve at parameter `s`, from the Bernstein form. */
Vec evaluate(const Curve & curve, long double s) {
  const long double r = 1.0L - s;
  long double b[4];
  Vec out{0, 0};

  if (curve.cubic) {
    b[0] = r * r * r;
    b[1] = 3 * s * r * r;
    b[2] = 3 * s * s * r;
    b[3] = s * s * s;
  }
  else {
    b[0] = r * r;
    b[1] = 2 * s * r;
    b[2] = s * s;
    b[3] = 0;
  }
  for (int i = 0; i < (curve.cubic ? 4 : 3); ++i) {
    out.x += b[i] * curve.p[i].x;
    out.y += b[i] * curve.p[i].y;
  }
  return out;
}

/** Distance from `q` to the segment `a`-`b`. */
long double distance(Vec q, GFNT_Point a, GFNT_Point b) {
  const long double ax = a.x;
  const long double ay = a.y;
  const long double dx = static_cast<long double>(b.x) - ax;
  const long double dy = static_cast<long double>(b.y) - ay;
  const long double length2 = dx * dx + dy * dy;
  long double along = 0;

  if (length2 > 0) {
    along = ((q.x - ax) * dx + (q.y - ay) * dy) / length2;
    along = along < 0 ? 0 : (along > 1 ? 1 : along);
  }
  const long double ex = q.x - (ax + along * dx);
  const long double ey = q.y - (ay + along * dy);
  return sqrtl(ex * ex + ey * ey);
}

constexpr int SAMPLES = 32;

/**
 * The largest distance from the curve to the polyline, taken over every piece:
 * thirty-three exact points in each, measured to that piece's segment.
 */
long double deviation(const Curve & curve, const std::vector<Vertex> & v) {
  long double worst = 0;
  GFNT_Point from = curve.p[0];

  for (const Vertex & vertex : v) {
    const long double span = std::ldexp(1.0L, vertex.level);

    for (int i = 0; i <= SAMPLES; ++i) {
      const long double s = (static_cast<long double>(vertex.leaf)
          + static_cast<long double>(i) / SAMPLES) / span;
      const long double d = distance(evaluate(curve, s), from, vertex.at);

      worst = d > worst ? d : worst;
    }
    from = vertex.at;
  }
  return worst;
}

/** xorshift64*: the same stream on every host. */
struct Random {
  uint64_t state;

  uint64_t next() {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 0x2545F4914F6CDD1DULL;
  }

  /** A coordinate in `[-2^bits, 2^bits)`, or the whole int32 range for 31. */
  GFNT_F26Dot6 coordinate(int bits) {
    const uint64_t span = uint64_t{1} << (bits + 1);
    const int64_t value = static_cast<int64_t>(next() % span)
        - (int64_t{1} << bits);

    return static_cast<GFNT_F26Dot6>(value);
  }
};

Curve random_curve(Random & random, bool cubic, int bits) {
  Curve curve{cubic, {}};

  for (int i = 0; i < (cubic ? 4 : 3); ++i) {
    curve.p[i] = {random.coordinate(bits), random.coordinate(bits)};
  }
  return curve;
}

/** Every control point at one of the four corners of the int32 square. */
Curve corner_curve(Random & random, bool cubic) {
  Curve curve{cubic, {}};

  for (int i = 0; i < (cubic ? 4 : 3); ++i) {
    curve.p[i] = {(random.next() & 1u) ? INT32_MAX : INT32_MIN_,
        (random.next() & 1u) ? INT32_MAX : INT32_MIN_};
  }
  return curve;
}

/** `t - 0.12`, the bound flatten.c proves, with the arithmetic's own slack. */
long double bound(GFNT_F26Dot6 tolerance) {
  return static_cast<long double>(tolerance) - 0.12L + 1e-4L;
}

/**
 * Flatten, measure, assert. `worst` is the smallest margin seen, the tolerance
 * minus the deviation: the proof says it is at least 0.12, so it is the number
 * to watch.
 */
void check(const Curve & curve, GFNT_F26Dot6 tolerance, long double & worst,
    size_t & most_vertices) {
  std::vector<Vertex> vertices;
  const int forced = flatten(curve, tolerance, vertices);
  const long double d = deviation(curve, vertices);

  EXPECT_EQ(forced, 0) << "the depth cap forced a piece, tolerance "
      << tolerance;
  EXPECT_LE(d, bound(tolerance)) << (curve.cubic ? "cubic" : "quadratic")
      << " at tolerance " << tolerance << " strays " << static_cast<double>(d)
      << " units; first point " << curve.p[0].x << "," << curve.p[0].y
      << " last " << vertices.back().at.x << "," << vertices.back().at.y;
  const long double margin = static_cast<long double>(tolerance) - d;

  worst = margin < worst ? margin : worst;
  most_vertices = vertices.size() > most_vertices ? vertices.size()
                                                  : most_vertices;
}

const GFNT_F26Dot6 TOLERANCES[] = {2, 3, 4, 5, 8, 16, 64, 1000};

}  // namespace

TEST(Flatten, TheRevisionAndTheSmallestToleranceAreWhatTheHeaderSays) {
  EXPECT_EQ(GFNT_RASTER_FLATTEN_REVISION, 2);
  EXPECT_EQ(GFNT_RASTER_MIN_TOLERANCE, 2);
}

/**
 * The property itself, at every size a curve can have. The classes are the
 * coordinate range `[-2^b, 2^b)`: 4 is a curve inside a quarter of a pixel's
 * cell, 16 is a few hundred pixels, 31 is the whole int32 range, which is
 * what a hostile outline can ask for and what flatten.c proves for.
 */
TEST(Flatten, EveryPieceOfEveryCurveIsWithinTheToleranceOfTheCurve) {
  Random random{0x9E3779B97F4A7C15ULL};
  const int classes[] = {4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30,
      31};

  for (const int bits : classes) {
    // Few big curves: a curve of the whole range is tens of thousands of
    // pieces at the smallest tolerance, each sampled thirty-three times.
    const int curves = bits <= 16 ? 24 : (bits <= 24 ? 6 : 3);
    long double worst = 1e9L;
    size_t most = 0;

    for (const GFNT_F26Dot6 tolerance : TOLERANCES) {
      for (int i = 0; i < curves; ++i) {
        check(random_curve(random, false, bits), tolerance, worst, most);
        check(random_curve(random, true, bits), tolerance, worst, most);
      }
    }
    // The smallest margin seen, per class, so a regression that eats the
    // margin shows in the log before it crosses the bound.
    printf("flatten: coordinates below 2^%d, smallest margin %.3f units, "
           "most vertices %zu\n", bits, static_cast<double>(worst), most);
  }
}

/** The curves that make the second differences as large as an int32 pair allows. */
TEST(Flatten, TheExtremeCurvesOfTheInt32SquareAreWithinTheTolerance) {
  Random random{0x0123456789ABCDEFULL};
  long double worst = 1e9L;
  size_t most = 0;

  for (const GFNT_F26Dot6 tolerance : {2, 4, 64}) {
    for (int i = 0; i < 4; ++i) {
      check(corner_curve(random, false), tolerance, worst, most);
      check(corner_curve(random, true), tolerance, worst, most);
    }
  }
  // The vectors below pin two specific ones; this is the random few.
  printf("flatten: int32 corners, smallest margin %.3f units, most vertices "
         "%zu\n",
      static_cast<double>(worst), most);
}

/** Small tolerances are where a constant threshold would fail; see flatten.c. */
TEST(Flatten, EveryToleranceIsHonouredOnTheSameCurves) {
  Random random{0xDEADBEEFCAFEF00DULL};

  for (GFNT_F26Dot6 tolerance = 2; tolerance <= 40; ++tolerance) {
    long double worst = 1e9L;
    size_t most = 0;

    for (int i = 0; i < 6; ++i) {
      check(random_curve(random, false, 16), tolerance, worst, most);
      check(random_curve(random, true, 16), tolerance, worst, most);
    }
  }
}

/**
 * Answers worked out by `tools/golden`-independent arithmetic: a Python model of
 * revision 2 using unbounded integers (see flatten.c for the rule), recorded
 * here as a vertex count, the first and last vertex and a hash of all of them.
 * They pin the rounding rules - ties upward at both the midpoints and the
 * narrowing, floors of negative values - which the distance test is too coarse to
 * notice (a floor midpoint stays within the bound on most curves).
 */
TEST(Flatten, TheVertexListsAreWhatTheIndependentModelSays) {
  struct Known {
    bool cubic;
    GFNT_F26Dot6 tolerance;
    GFNT_Point p[4];
    size_t count;
    uint64_t hash;
    GFNT_Point first;
    GFNT_Point last;
  };
  const Known known[] = {
{false, 4, {{0,0},{3001,4999},{8003,1}}, 32, 0x5e9fe3e61234e30bULL, {190,303}, {8003,1}},
    {false, 4, {{-1000,-777},{-3001,5001},{777,-9999}}, 64, 0x4bbe0f147b91fd7dULL, {-1061,-602}, {777,-9999}},
    {false, 2, {{0,0},{400,-1200},{800,0}}, 32, 0xc71abb9947fcae38ULL, {25,-73}, {800,0}},
    {true, 4, {{0,0},{1000,3001},{-2001,4001},{5000,-7}}, 59, 0xdd37fa8b0432a31fULL, {44,139}, {5000,-7}},
    {true, 3, {{-31,17},{-8003,2999},{9001,-6001},{64,-64}}, 109, 0x29ecf687a1bbf682ULL, {-213,85}, {64,-64}},
    {false, 64, {{INT32_MIN,INT32_MAX},{INT32_MAX,INT32_MIN},{INT32_MIN,INT32_MIN}}, 8192, 0x1d73f31c84978d9dULL, {-2146435200,2146435135}, {INT32_MIN,INT32_MIN}},
    {true, 2, {{INT32_MIN,INT32_MIN},{INT32_MAX,INT32_MIN},{INT32_MIN,INT32_MAX},{INT32_MAX,INT32_MAX}}, 97787, 0xea23931ffec1d46bULL, {-2147385345,-INT32_MAX}, {INT32_MAX,INT32_MAX}},
  };

  for (const Known & k : known) {
    Curve curve{k.cubic, {k.p[0], k.p[1], k.p[2], k.p[3]}};
    std::vector<Vertex> vertices;
    uint64_t hash = 0xCBF29CE484222325ULL;

    EXPECT_EQ(flatten(curve, k.tolerance, vertices), 0);
    ASSERT_EQ(vertices.size(), k.count);
    for (const Vertex & vertex : vertices) {
      for (const int32_t value : {vertex.at.x, vertex.at.y}) {
        const uint32_t bits = static_cast<uint32_t>(value);

        for (int shift = 0; shift < 32; shift += 8) {
          hash = (hash ^ ((bits >> shift) & 0xFFu)) * 0x100000001B3ULL;
        }
      }
    }
    EXPECT_EQ(hash, k.hash);
    EXPECT_EQ(vertices.front().at.x, k.first.x);
    EXPECT_EQ(vertices.front().at.y, k.first.y);
    EXPECT_EQ(vertices.back().at.x, k.last.x);
    EXPECT_EQ(vertices.back().at.y, k.last.y);
  }
}

/**
 * The depth cap, which the proof says no input reaches, is exercised with a
 * smaller one: a piece it forces is counted, still emitted, and the pieces still
 * tile the curve. Without this the count is a number nothing has ever seen move.
 */
TEST(Flatten, APieceTheCapForcesIsCountedAndStillTilesTheCurve) {
  const GFNT_Point quad[3] = {{0, 0}, {1 << 20, 1 << 22}, {1 << 21, 0}};
  const GFNT_Point cubic[4] = {{0, 0}, {1 << 20, 1 << 22}, {-(1 << 21), 1 << 21},
      {1 << 21, 0}};

  for (const bool is_cubic : {false, true}) {
    for (const int levels : {1, 3, 6}) {
      std::vector<Vertex> vertices;
      const int forced = is_cubic
          ? gfnt_flatten_cubic_capped(4, levels, cubic[0], cubic[1], cubic[2],
                cubic[3], collect, &vertices)
          : gfnt_flatten_quad_capped(4, levels, quad[0], quad[1], quad[2],
                collect, &vertices);
      uint64_t next = 0;

      EXPECT_GT(forced, 0) << "levels " << levels;
      EXPECT_EQ(static_cast<size_t>(forced), vertices.size())
          << "every piece of a capped curve this large is forced";
      for (const Vertex & vertex : vertices) {
        EXPECT_EQ(vertex.level, levels);
        EXPECT_EQ(static_cast<uint64_t>(vertex.leaf), next);
        next += 1;
      }
      EXPECT_EQ(next, uint64_t{1} << levels);
    }
  }
  // The same cap applied to a curve it does not bind counts nothing.
  std::vector<Vertex> vertices;
  const GFNT_Point flat[3] = {{0, 0}, {320, 160}, {640, 320}};

  EXPECT_EQ(gfnt_flatten_quad_capped(4, 1, flat[0], flat[1], flat[2], collect,
                &vertices), 0);
}

/** The pieces tile the parameter interval, in order, with no gap or overlap. */
TEST(Flatten, ThePiecesTileTheCurveInOrderAndEndAtTheLastPoint) {
  Random random{0x5555AAAA5555AAAAULL};

  for (const int bits : {6, 14, 22, 31}) {
    for (const bool cubic : {false, true}) {
      const Curve curve = random_curve(random, cubic, bits);
      std::vector<Vertex> vertices;
      uint64_t next = 0;

      flatten(curve, 4, vertices);
      ASSERT_FALSE(vertices.empty());
      for (const Vertex & vertex : vertices) {
        ASSERT_LE(vertex.level, GFNT_FLATTEN_DEPTH);
        const uint64_t start = static_cast<uint64_t>(vertex.leaf)
            << (GFNT_FLATTEN_DEPTH - vertex.level);
        const uint64_t width = uint64_t{1}
            << (GFNT_FLATTEN_DEPTH - vertex.level);

        ASSERT_EQ(start, next);
        next = start + width;
      }
      EXPECT_EQ(next, uint64_t{1} << GFNT_FLATTEN_DEPTH);
      const GFNT_Point last = curve.p[cubic ? 3 : 2];

      EXPECT_EQ(vertices.back().at.x, last.x);
      EXPECT_EQ(vertices.back().at.y, last.y);
    }
  }
}

TEST(Flatten, AFlatCurveIsOnePieceAndADegenerateOneIsToo) {
  std::vector<Vertex> vertices;
  const Curve straight{false, {{0, 0}, {320, 160}, {640, 320}, {0, 0}}};
  const Curve point{true, {{-7, 9}, {-7, 9}, {-7, 9}, {-7, 9}}};

  EXPECT_EQ(flatten(straight, 4, vertices), 0);
  EXPECT_EQ(vertices.size(), 1u);
  EXPECT_EQ(flatten(point, 4, vertices), 0);
  ASSERT_EQ(vertices.size(), 1u);
  EXPECT_EQ(vertices[0].at.x, -7);
  EXPECT_EQ(vertices[0].at.y, 9);
}

/** A tolerance below the minimum is raised to it by the flattener's guard. */
TEST(Flatten, ATooSmallToleranceIsRaisedToTheMinimumNotTrusted) {
  const Curve curve{false, {{0, 0}, {900, 3000}, {2000, -100}, {0, 0}}};
  std::vector<Vertex> low;
  std::vector<Vertex> minimum;

  for (const GFNT_F26Dot6 tolerance : {GFNT_F26Dot6{-5}, GFNT_F26Dot6{0},
           GFNT_F26Dot6{1}}) {
    flatten(curve, tolerance, low);
    flatten(curve, GFNT_RASTER_MIN_TOLERANCE, minimum);
    ASSERT_EQ(low.size(), minimum.size());
    for (size_t i = 0; i < low.size(); ++i) {
      EXPECT_EQ(low[i].at.x, minimum[i].at.x);
      EXPECT_EQ(low[i].at.y, minimum[i].at.y);
    }
  }
}

/** The public door: the option is a promise, so a promise it cannot keep is refused. */
TEST(Flatten, TheRasteriserRefusesATolerancePastWhatItCanKeep) {
  GFNT_Outline * outline = nullptr;
  GFNT_Error error{};
  GFNT_Coverage coverage{};
  GFNT_RasterOptions options{};

  ASSERT_EQ(gfnt_outline_create(nullptr, &outline, &error), GFNT_OK);
  const GFNT_Point square[] = {{0, 0}, {64 * 8, 0}, {64 * 8, 64 * 8},
      {0, 64 * 8}};
  ASSERT_EQ(gfnt_outline_begin_contour(outline, &error), GFNT_OK);
  for (const GFNT_Point & point : square) {
    ASSERT_EQ(gfnt_outline_add_point(outline, point, GFNT_POINT_ON, &error),
        GFNT_OK);
  }
  ASSERT_EQ(gfnt_outline_scale(outline, GFNT_F16DOT16_ONE, &error), GFNT_OK);

  for (const GFNT_F26Dot6 refused : {GFNT_F26Dot6{-1}, GFNT_F26Dot6{1}}) {
    options.tolerance = refused;
    EXPECT_EQ(gfnt_raster_outline(outline, &options, nullptr, nullptr,
                  &coverage, &error), GFNT_ERR_INVALID)
        << "tolerance " << refused;
  }
  for (const GFNT_F26Dot6 accepted : {GFNT_F26Dot6{0}, GFNT_F26Dot6{2},
           GFNT_F26Dot6{4}, GFNT_F26Dot6{INT32_MAX}}) {
    options.tolerance = accepted;
    EXPECT_EQ(gfnt_raster_outline(outline, &options, nullptr, nullptr,
                  &coverage, &error), GFNT_OK)
        << "tolerance " << accepted;
    gfnt_coverage_destroy(&coverage);
  }
  gfnt_outline_destroy(outline);
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
