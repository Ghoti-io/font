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
 * The built-in band callback for exclusions.
 */

#include <ghoti.io/font/layout.h>
#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/outline.h>
#include <ghoti.io/unicode/bidi.h>
#include <ghoti.io/unicode/break.h>
#include <ghoti.io/unicode/case.h>
#include <ghoti.io/unicode/char.h>
#include <ghoti.io/unicode/norm.h>
#include <ghoti.io/unicode/script.h>
#include <ghoti.io/unicode/utf.h>
#include <stdlib.h>
#include <string.h>
#include "kashida.h"
#include "layout_internal.h"

/*
 * Every quantity here is an integer and every result is one, so the arithmetic
 * is integer arithmetic, exact on every target (AD-22). Coordinates, sizes and
 * margins are int32; sums of a few of them fit int64; the only products wider
 * than that are the two below, and each fits uint64 because its factors are
 * bounded by the validation in gfnt_exclusions_band(): a circle's radius plus
 * margin is under 2^32 (its box lies inside the int32 range), and a polygon
 * edge's two factors are each under 2^32 (a difference of two int32).
 */

/** The smallest integer whose square is at least @p n: ceil(sqrt(n)), exact. */
static uint64_t flow_isqrt_ceil(uint64_t n) {
  uint64_t root = 0;
  uint64_t bit = (uint64_t)1 << 62;

  while (bit > n) bit >>= 2;
  // The floor of the square root, a bit at a time: no division, no float.
  for (uint64_t rest = n; bit; bit >>= 2) {
    if (rest >= root + bit) {
      rest -= root + bit;
      root = (root >> 1) + bit;
    }
    else {
      root >>= 1;
    }
  }
  return root * root < n ? root + 1 : root;
}

/**
 * The floor and the ceiling of `a + (num / den)` for `num` and `den` positive,
 * and a sign: `a + sign * (num * span) / den`, the point `num / den` of the way
 * from `a` along a run of `sign * span`. @p num is at most @p den, @p span is
 * under 2^32 and so is @p den, so the product is under 2^64.
 */
static void flow_cross(int64_t a, bool negative, uint64_t num, uint64_t span,
    uint64_t den, int64_t * floor_x, int64_t * ceil_x) {
  const uint64_t product = num * span;
  const int64_t whole = (int64_t)(product / den);
  const int64_t part = product % den ? 1 : 0;

  if (negative) {
    *floor_x = a - whole - part;
    *ceil_x = a - whole;
  }
  else {
    *floor_x = a + whole;
    *ceil_x = a + whole + part;
  }
}

/** Widen the interval `[lo, hi]` to hold `[x_floor, x_ceil]`. */
static void flow_include_x(int64_t x_floor, int64_t x_ceil, int64_t * lo,
    int64_t * hi, bool * found) {
  if (!*found || x_floor < *lo) *lo = x_floor;
  if (!*found || x_ceil > *hi) *hi = x_ceil;
  *found = true;
}

/**
 * Bounds of a polygon clipped to a horizontal slab: the least floor and the
 * greatest ceiling of the x at which its edges meet the slab's two sides or
 * have a vertex inside it (the floor and the ceiling of a minimum and of a
 * maximum are the minimum of the floors and the maximum of the ceilings, as
 * floor and ceiling are monotone).
 */
static bool flow_polygon_slab(const GFNT_FlowPoint * points, size_t count,
    int64_t y0, int64_t y1, int64_t * x0, int64_t * x1, int64_t * bottom) {
  bool found = false;
  int64_t lo = 0, hi = 0, min_y = points[0].y, max_y = points[0].y;
  for (size_t i = 1; i < count; i++) {
    if (points[i].y < min_y) min_y = points[i].y;
    if (points[i].y > max_y) max_y = points[i].y;
  }
  if (y1 <= min_y || y0 >= max_y) return false;
  for (size_t i = 0; i < count; i++) {
    const GFNT_FlowPoint a = points[i], b = points[(i + 1) % count];
    const int64_t ay = a.y, by = b.y;
    if (ay > max_y) max_y = ay;
    if (ay >= y0 && ay <= y1) flow_include_x(a.x, a.x, &lo, &hi, &found);
    if (ay != by) {
      const int64_t low = ay < by ? ay : by;
      const int64_t high = ay > by ? ay : by;
      const int64_t scans[] = {y0, y1};
      for (size_t k = 0; k < 2; k++) {
        const int64_t y = scans[k];
        if (y >= low && y <= high) {
          // x = a.x + (y - ay) * (b.x - a.x) / (by - ay), with the ratio
          // taken positive: its numerator and denominator have one sign.
          const int64_t dx = (int64_t)b.x - a.x;
          int64_t x_floor, x_ceil;
          flow_cross(a.x, dx < 0, (uint64_t)(by > ay ? y - ay : ay - y),
              (uint64_t)(dx < 0 ? -dx : dx), (uint64_t)(high - low), &x_floor,
              &x_ceil);
          flow_include_x(x_floor, x_ceil, &lo, &hi, &found);
        }
      }
    }
  }
  if (!found) return false;
  *x0 = lo; *x1 = hi; *bottom = max_y;
  return true;
}

/**
 * Bounds of one exclusion over the line's cross-axis interval: the x range as
 * whole units (the floor of the left edge, the ceiling of the right), and the
 * y below which the exclusion ends.
 */
static bool flow_exclusion_slab(const GFNT_Exclusion * item, GFNT_F26Dot6 top,
    GFNT_F26Dot6 height, int64_t * x0, int64_t * x1, int64_t * bottom) {
  const int64_t margin = item->margin;
  const int64_t y0 = (int64_t)top - margin;
  const int64_t y1 = (int64_t)top + height + margin;
  int64_t lo = 0, hi = 0, lower = 0;
  switch (item->shape) {
    case GFNT_EXCLUSION_RECTANGLE: {
      const GFNT_Rect r = item->geometry.rectangle;
      const int64_t upper = (int64_t)r.y - margin;
      lower = (int64_t)r.y + r.height + margin;
      if (y1 <= upper || y0 >= lower) return false;
      lo = (int64_t)r.x - margin;
      hi = (int64_t)r.x + r.width + margin;
      break;
    }
    case GFNT_EXCLUSION_CIRCLE: {
      const GFNT_FlowPoint center = item->geometry.circle.center;
      const int64_t radius = (int64_t)item->geometry.circle.radius + margin;
      const int64_t top_edge = (int64_t)center.y - radius;
      lower = (int64_t)center.y + radius;
      if (y1 <= top_edge || y0 >= lower) return false;
      // The query overlaps the open interval (center.y - radius, center.y +
      // radius), so the nearest y of the slab is less than a radius from the
      // center and the square under the root is positive (zero for a radius
      // of nothing). The circle's half-width there is sqrt(radius^2 - dy^2),
      // which is not an integer in general: the left edge is the floor of
      // center.x less it, which is center.x less its ceiling, and the right
      // edge likewise.
      int64_t dy = 0;
      if ((int64_t)center.y < y0) dy = y0 - center.y;
      else if ((int64_t)center.y > y1) dy = center.y - y1;
      const uint64_t reach = flow_isqrt_ceil(
          (uint64_t)radius * (uint64_t)radius - (uint64_t)dy * (uint64_t)dy);
      lo = (int64_t)center.x - (int64_t)reach;
      hi = (int64_t)center.x + (int64_t)reach;
      break;
    }
    case GFNT_EXCLUSION_POLYGON:
      if (!flow_polygon_slab(item->geometry.polygon.points, item->geometry.polygon.count,
              y0, y1, &lo, &hi, &lower)) return false;
      lo -= margin; hi += margin; lower += margin;
      break;
    default:
      return false;
  }
  if (lo < INT32_MIN || hi > INT32_MAX || lower < INT32_MIN || lower > INT32_MAX) return false;
  *x0 = lo; *x1 = hi; *bottom = lower;
  return true;
}

GFNT_Result gfnt_exclusions_band(void * ctx, GFNT_F26Dot6 top,
    GFNT_F26Dot6 height, GFNT_Band * out) {
  const GFNT_Exclusions * exclusions = ctx;
  int64_t start, end, next_top;
  bool blocked = false;
  if (!exclusions || !out || top < 0 || height < 0 || exclusions->width < 0
      || (exclusions->count && !exclusions->items)) return GFNT_ERR_INVALID;
  start = exclusions->start;
  end = start + (int64_t)exclusions->width;
  next_top = top;
  if (start < INT32_MIN || start > INT32_MAX || end < INT32_MIN || end > INT32_MAX) {
    return GFNT_ERR_INVALID;
  }
  for (size_t i = 0; i < exclusions->count; i++) {
    const GFNT_Exclusion * item = &exclusions->items[i];
    int64_t left, right;
    int64_t bottom;
    bool affects = false;
    if (item->margin < 0 || item->side < GFNT_EXCLUSION_TEXT_RIGHT
        || item->side > GFNT_EXCLUSION_TEXT_LEFT || item->shape < GFNT_EXCLUSION_RECTANGLE
        || item->shape > GFNT_EXCLUSION_POLYGON) return GFNT_ERR_INVALID;
    if (item->shape == GFNT_EXCLUSION_RECTANGLE) {
      const GFNT_Rect r = item->geometry.rectangle;
      const int64_t right_edge = (int64_t)r.x + r.width + item->margin;
      const int64_t bottom_edge = (int64_t)r.y + r.height + item->margin;
      if (r.width < 0 || r.height < 0 || (int64_t)r.x - item->margin < INT32_MIN
          || right_edge > INT32_MAX || (int64_t)r.y - item->margin < INT32_MIN
          || bottom_edge > INT32_MAX) return GFNT_ERR_INVALID;
    }
    if (item->shape == GFNT_EXCLUSION_CIRCLE) {
      const int64_t radius = (int64_t)item->geometry.circle.radius + item->margin;
      const GFNT_FlowPoint center = item->geometry.circle.center;
      if (item->geometry.circle.radius < 0 || (int64_t)center.x - radius < INT32_MIN
          || (int64_t)center.x + radius > INT32_MAX || (int64_t)center.y - radius < INT32_MIN
          || (int64_t)center.y + radius > INT32_MAX) return GFNT_ERR_INVALID;
    }
    if (item->shape == GFNT_EXCLUSION_POLYGON
        && (!item->geometry.polygon.points || item->geometry.polygon.count < 3)) {
      return GFNT_ERR_INVALID;
    }
    if (item->shape == GFNT_EXCLUSION_POLYGON) {
      for (size_t j = 0; j < item->geometry.polygon.count; j++) {
        const GFNT_FlowPoint point = item->geometry.polygon.points[j];
        if ((int64_t)point.x - item->margin < INT32_MIN
            || (int64_t)point.x + item->margin > INT32_MAX
            || (int64_t)point.y - item->margin < INT32_MIN
            || (int64_t)point.y + item->margin > INT32_MAX) return GFNT_ERR_INVALID;
      }
    }
    if (!flow_exclusion_slab(item, top, height, &left, &right, &bottom)) continue;
    if (item->side == GFNT_EXCLUSION_TEXT_RIGHT) {
      if (right > start) {
        start = right;
        affects = true;
      }
    }
    else if (left < end) {
      end = left;
      affects = true;
    }
    if (affects && bottom > top) {
      if (!blocked || bottom < next_top) next_top = bottom;
      blocked = true;
    }
  }
  if (start < INT32_MIN || start > INT32_MAX || end < INT32_MIN || end > INT32_MAX
      || end - start > INT32_MAX || next_top > INT32_MAX) return GFNT_ERR_INVALID;
  out->start = (GFNT_F26Dot6)start;
  out->width = end > start ? (GFNT_F26Dot6)(end - start) : 0;
  out->next_top = blocked ? (GFNT_F26Dot6)next_top : top;
  return GFNT_OK;
}
