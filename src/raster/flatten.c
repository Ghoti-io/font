/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Font.
 *
 * Ghoti.io Font is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Font is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * The curve flattener, revision 2, and the proof that its polyline stays within
 * the tolerance of the curve.
 *
 * documentation/design.md section 8.1.
 *
 * ## What is done
 *
 * A curve is halved at its midpoint until its control polygon is flat, and the
 * end point of each flat piece is a vertex. Three things make the bound below
 * true, and each of them was a defect of revision 1:
 *
 * 1. **The subdivision is done in widened integers**: every point is multiplied
 *    by 2^::GFNT_FLATTEN_EXTRA (256) on entry and all of the halving and both
 *    flatness tests run on those. Revision 1 rounded every midpoint to 26.6, so
 *    each level moved the control points of every later level and the errors
 *    added up with the depth: measured against the exact curve it reached 13
 *    units (1/5 pixel) on a large curve, where 4 was promised.
 * 2. **A vertex is rounded to 26.6 once**, when it is emitted, to nearest with
 *    ties upward. That is the only rounding of a coordinate that is not
 *    2^-9 of a unit.
 * 3. **The flatness threshold leaves room for that one rounding**: a piece is flat
 *    when its L1 deviation sum is at most `4 * 256 * (t - 1)` widened units,
 *    i.e. `4 * (t - 1)` units, for a tolerance of `t` units. Revision 1 spent the
 *    whole tolerance on the flatness test (`4 * t`) and so had none to spare for
 *    rounding.
 *
 * No float, no libm, no shift of a negative value (a widening is a
 * multiplication, a narrowing a floor division), no overflow: the next section
 * proves the ranges. The result is the same on every target.
 *
 * ## The theorem
 *
 * Let the curve have control points with int32 coordinates (any value of the
 * type, not only those a glyph produces), and let `t >= 2` be the tolerance, in
 * 26.6 units (so `t = 4` is the default 1/16 pixel). Then every point of the
 * curve is within `t - 0.12` units (hence within `t`) of the emitted polyline,
 * and the depth cap never forces a piece.
 *
 * ## The proof
 *
 * Units are 26.6 units unless called "widened" (1 widened = 1/256 unit). Write
 * W = 256, T = 4W(t-1), E = 30.
 *
 * **(a) Ranges.** Every coordinate of every point the recursion makes lies in
 * the interval spanned by the corresponding input coordinates, widened, i.e.
 * within [-2^31 W, (2^31-1) W] and so within +-2^39 widened: the inputs are
 * there, and the half-up midpoint `floor((u+v+1)/2)` of two integers in an
 * interval `[lo, hi]` is in it (the sum is at most `2 hi + 1`, whose half
 * floors to `hi`). Hence 3p, 2p and sums of up to six such terms are below
 * 2^42, which is far inside int64. A quadratic's `|2p1-p0-p2|` is at most
 * 2^33 per axis in units; a cubic's `|3p1-2p0-p3|` at most 2^34. Narrowing
 * `floor((v + 128) / 256)` of a widened value in that interval returns a value
 * in [-2^31, 2^31-1] (monotonic, and exact at the end points), so the cast to
 * int32 does not truncate. The threshold is `4W(t-1)` with `t - 1 < 2^31`: below
 * 2^41. No statement here depends on the input being a glyph.
 *
 * **(b) The midpoints are close to the true ones.** Take the control points Q
 * of a piece reached by `k` halvings and the control points P of the same
 * parameter interval of the exact curve (the exact de Casteljau points, which
 * are rational). If every point of the parent is within `e` of its exact
 * counterpart on each axis, each new point is: it is the average of two parent
 * points (error at most `e`, an average does not magnify) plus the half-up
 * rounding, at most 1/2 widened. A quadratic produces `a, b` (error +1/2) and
 * `m = mid(a, b)` (+1); a cubic `a, b, c` (+1/2), `d, e` (+1) and `m` (+3/2).
 * The root's points are exact (`e = 0`). So after `k <= GFNT_FLATTEN_DEPTH = 20`
 * halvings every point is within `1.5 * 20 = 30 = E` widened per axis, for
 * either degree.
 *
 * **(c) The flatness test measures the piece it has.** The test is made on Q.
 * For a quadratic piece, `B(s) - L(s) = s(1-s)(2Q1 - Q0 - Q2)`, where L is the
 * chord, because the Bernstein form minus linear interpolation leaves only the
 * middle coefficient. So the piece is within `|2Q1-Q0-Q2|_2 / 4 <= dx+dy / 4` of
 * the chord, and the test accepts at `dx+dy <= T`: within `T/4 = W(t-1)`
 * widened, which is `t - 1` units. For a cubic, with
 * `e1 = Q1 - (2Q0+Q3)/3` and `e2 = Q2 - (Q0+2Q3)/3`, the same identity gives
 * `B(s) - L(s) = 3s(1-s)((1-s) e1 + s e2)`, at most `(3/4) max(|e1|, |e2|)`; and
 * `|e1|_2 <= |e1|_1 = d1/3`, `|e2|_2 <= d2/3` for the sums d1, d2 the code
 * computes. So the piece is within `max(d1, d2) / 4 <= T/4` as well: `t - 1`
 * units. (The distance is to the chord at the same parameter, hence to the
 * chord segment.)
 *
 * **(d) The piece is near the exact curve.** The piece's Bezier curve, built on
 * Q, and the exact curve over the same interval, built on P, are the same
 * convex combination of Q and of P at every parameter, so they are within
 * `max |Q_i - P_i|_2 <= sqrt(2) E / 256 <= 0.166` units of each other at the same
 * parameter, by (b).
 *
 * **(e) The vertex rounding.** The segment actually emitted joins
 * `narrow(Q0)` and `narrow(Qk)`: the previous vertex is the narrowing of the
 * same point (it is the shared `m`; for the first piece it is the input point,
 * which narrows to itself). Each narrowing moves a point by at most 1/2 unit
 * per axis, so at most `sqrt(2)/2 = 0.7072` units, and a point of the chord is a
 * convex combination of the two ends, so it moves by at most that.
 *
 * **(f) Together**, for any point B(s) of the curve and the piece containing s:
 * `0.166 (d) + (t - 1) (c) + 0.7072 (e) = t - 0.1268 < t - 0.12`. QED, given
 * that the cap does not bind:
 *
 * **(g) The depth cap does not bind** for `t >= 2`, over the whole int32 range.
 * A piece is forced only after 20 halvings; its control polygon then has the
 * exact deviation of the sub-curve plus the error of (b). The exact part: a
 * quadratic's `2Q1-Q0-Q2` for a piece of parameter length `h` is
 * `h^2 (P0 - 2P1 + P2)` (per axis at most `2^33 h^2` units), so with
 * `h = 2^-20` it is at most `2^34 / 2^40` units for both axes together:
 * 4 widened. A cubic's `3Q1-2Q0-Q3` is the negative of a Taylor remainder of
 * the piece, `<= (h^2/2) max |B''|` per axis, and `|B''| <= 6 * 2^33` per axis
 * for int32 control points, so `d1` and `d2` are each at most
 * `2 * 3 * 2^33 * 2^-40` units = 12 widened. The error part: coefficients
 * (2,1,1) and (3,2,1) times `E` per axis, both axes: at most `8 * 20 = 160`
 * widened for a quadratic and `12 * 30 = 360` for a cubic. The most is 372,
 * and the smallest threshold, at `t = 2`, is `T = 1024`. So every piece is
 * accepted at or before depth 20. (This is also why the depth is 20 and was 16:
 * at 16 a curve spanning the int32 range is still 4 (quadratic) or 12 (cubic)
 * units of deviation sum from flat, over the threshold of a small tolerance.)
 *
 * **Tolerances.** The bound is `t` for every accepted tolerance because the
 * threshold is derived from `t`: `4(t-1)` units rather than a constant. What
 * cannot be avoided is the rounding, `0.7072 + 0.166 = 0.873` units, so a
 * tolerance of 1 (1/64 pixel) cannot be met by any threshold the noise of (b)
 * leaves workable: the flatness part would have to be 0.127 units, a sum limit
 * under half a unit, below the 372 widened of error part (g) shows a
 * piece can carry at the depth cap. Two is the smallest value for which the
 * derivation above is closed, so ::GFNT_RASTER_MIN_TOLERANCE is 2 and the
 * rasteriser refuses a smaller one (revision 1 accepted 1 and gave the bound
 * `0.75 t + 0.81` below 4, which is not the tolerance asked for).
 *
 * ## What checks the proof
 *
 * `tests/unit/test_flatten.cpp` evaluates the curve exactly and measures the
 * distance to the polyline, on every size class and several tolerances, and
 * asserts the bound and a zero cap count. That it finds a violation was shown by
 * planting: a floor midpoint, no extra bits, a threshold of `4t`, a
 * floor narrowing and a depth of 16 each fail it.
 */

#include <ghoti.io/font/raster.h>
#include <stdint.h>
#include "../core/fixed.h"
#include "flatten.h"

/** Extra fractional bits the subdivision carries beyond 26.6. */
#define GFNT_FLATTEN_EXTRA 8

#define GFNT_FLATTEN_SCALE ((int64_t)1 << GFNT_FLATTEN_EXTRA)

/** A point in widened units: 26.6 times 2^::GFNT_FLATTEN_EXTRA. */
typedef struct GFNT_WidePoint {
  int64_t x;
  int64_t y;
} GFNT_WidePoint;

typedef struct GFNT_Flattener {
  int64_t threshold;  ///< The L1 flatness limit, widened: 4 * 256 * (t - 1).
  GFNT_FlattenEmit emit;
  void * user;
  int levels;         ///< The depth cap: at most GFNT_FLATTEN_DEPTH.
  int forced;         ///< Pieces the depth cap forced.
} GFNT_Flattener;

static GFNT_WidePoint gfnt_flatten_widen(GFNT_Point point) {
  GFNT_WidePoint out;

  // A multiplication, not a shift: the operand may be negative.
  out.x = (int64_t)point.x * GFNT_FLATTEN_SCALE;
  out.y = (int64_t)point.y * GFNT_FLATTEN_SCALE;
  return out;
}

/** A widened point back to 26.6: to nearest, ties upward. */
static GFNT_Point gfnt_flatten_narrow(GFNT_WidePoint point) {
  GFNT_Point out;

  out.x = (GFNT_F26Dot6)gfnt_floor_div(point.x + GFNT_FLATTEN_SCALE / 2,
      GFNT_FLATTEN_SCALE);
  out.y = (GFNT_F26Dot6)gfnt_floor_div(point.y + GFNT_FLATTEN_SCALE / 2,
      GFNT_FLATTEN_SCALE);
  return out;
}

/** The midpoint of two widened points: to nearest, ties upward. */
static GFNT_WidePoint gfnt_flatten_mid(GFNT_WidePoint a, GFNT_WidePoint b) {
  GFNT_WidePoint mid;

  mid.x = gfnt_floor_div(a.x + b.x + 1, 2);
  mid.y = gfnt_floor_div(a.y + b.y + 1, 2);
  return mid;
}

static int64_t gfnt_flatten_abs(int64_t value) {
  return value < 0 ? -value : value;
}

static void gfnt_flatten_leaf(GFNT_Flattener * flattener, GFNT_WidePoint to,
    uint32_t leaf, int level) {
  flattener->emit(flattener->user, gfnt_flatten_narrow(to), leaf, level);
}

static void gfnt_flatten_quad_piece(GFNT_Flattener * flattener,
    GFNT_WidePoint p0, GFNT_WidePoint p1, GFNT_WidePoint p2, uint32_t leaf,
    int level) {
  const int64_t sum = gfnt_flatten_abs(2 * p1.x - p0.x - p2.x)
      + gfnt_flatten_abs(2 * p1.y - p0.y - p2.y);

  if (sum <= flattener->threshold) {
    gfnt_flatten_leaf(flattener, p2, leaf, level);
    return;
  }
  if (level >= flattener->levels) {
    flattener->forced += 1;
    gfnt_flatten_leaf(flattener, p2, leaf, level);
    return;
  }
  {
    const GFNT_WidePoint a = gfnt_flatten_mid(p0, p1);
    const GFNT_WidePoint b = gfnt_flatten_mid(p1, p2);
    const GFNT_WidePoint m = gfnt_flatten_mid(a, b);

    gfnt_flatten_quad_piece(flattener, p0, a, m, leaf * 2u, level + 1);
    gfnt_flatten_quad_piece(flattener, m, b, p2, leaf * 2u + 1u, level + 1);
  }
}

static void gfnt_flatten_cubic_piece(GFNT_Flattener * flattener,
    GFNT_WidePoint p0, GFNT_WidePoint p1, GFNT_WidePoint p2, GFNT_WidePoint p3,
    uint32_t leaf, int level) {
  const int64_t d1 = gfnt_flatten_abs(3 * p1.x - 2 * p0.x - p3.x)
      + gfnt_flatten_abs(3 * p1.y - 2 * p0.y - p3.y);
  const int64_t d2 = gfnt_flatten_abs(3 * p2.x - p0.x - 2 * p3.x)
      + gfnt_flatten_abs(3 * p2.y - p0.y - 2 * p3.y);

  if (d1 <= flattener->threshold && d2 <= flattener->threshold) {
    gfnt_flatten_leaf(flattener, p3, leaf, level);
    return;
  }
  if (level >= flattener->levels) {
    flattener->forced += 1;
    gfnt_flatten_leaf(flattener, p3, leaf, level);
    return;
  }
  {
    const GFNT_WidePoint a = gfnt_flatten_mid(p0, p1);
    const GFNT_WidePoint b = gfnt_flatten_mid(p1, p2);
    const GFNT_WidePoint c = gfnt_flatten_mid(p2, p3);
    const GFNT_WidePoint d = gfnt_flatten_mid(a, b);
    const GFNT_WidePoint e = gfnt_flatten_mid(b, c);
    const GFNT_WidePoint m = gfnt_flatten_mid(d, e);

    gfnt_flatten_cubic_piece(flattener, p0, a, d, m, leaf * 2u, level + 1);
    gfnt_flatten_cubic_piece(flattener, m, e, c, p3, leaf * 2u + 1u,
        level + 1);
  }
}

static GFNT_Flattener gfnt_flatten_begin(GFNT_F26Dot6 tolerance, int levels,
    GFNT_FlattenEmit emit, void * user) {
  GFNT_Flattener flattener;
  const int64_t t = tolerance < GFNT_RASTER_MIN_TOLERANCE
      ? GFNT_RASTER_MIN_TOLERANCE : tolerance;

  flattener.threshold = 4 * GFNT_FLATTEN_SCALE * (t - 1);
  flattener.emit = emit;
  flattener.user = user;
  flattener.levels = levels < 1 ? 1
      : (levels > GFNT_FLATTEN_DEPTH ? GFNT_FLATTEN_DEPTH : levels);
  flattener.forced = 0;
  return flattener;
}

int gfnt_flatten_quad_capped(GFNT_F26Dot6 tolerance, int levels, GFNT_Point p0,
    GFNT_Point p1, GFNT_Point p2, GFNT_FlattenEmit emit, void * user) {
  GFNT_Flattener flattener = gfnt_flatten_begin(tolerance, levels, emit, user);

  gfnt_flatten_quad_piece(&flattener, gfnt_flatten_widen(p0),
      gfnt_flatten_widen(p1), gfnt_flatten_widen(p2), 0u, 0);
  return flattener.forced;
}

int gfnt_flatten_cubic_capped(GFNT_F26Dot6 tolerance, int levels, GFNT_Point p0,
    GFNT_Point p1, GFNT_Point p2, GFNT_Point p3, GFNT_FlattenEmit emit,
    void * user) {
  GFNT_Flattener flattener = gfnt_flatten_begin(tolerance, levels, emit, user);

  gfnt_flatten_cubic_piece(&flattener, gfnt_flatten_widen(p0),
      gfnt_flatten_widen(p1), gfnt_flatten_widen(p2), gfnt_flatten_widen(p3),
      0u, 0);
  return flattener.forced;
}

int gfnt_flatten_quad(GFNT_F26Dot6 tolerance, GFNT_Point p0, GFNT_Point p1,
    GFNT_Point p2, GFNT_FlattenEmit emit, void * user) {
  return gfnt_flatten_quad_capped(tolerance, GFNT_FLATTEN_DEPTH, p0, p1, p2,
      emit, user);
}

int gfnt_flatten_cubic(GFNT_F26Dot6 tolerance, GFNT_Point p0, GFNT_Point p1,
    GFNT_Point p2, GFNT_Point p3, GFNT_FlattenEmit emit, void * user) {
  return gfnt_flatten_cubic_capped(tolerance, GFNT_FLATTEN_DEPTH, p0, p1, p2,
      p3, emit, user);
}
