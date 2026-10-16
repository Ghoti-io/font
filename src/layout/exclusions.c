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
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "kashida.h"
#include "layout_internal.h"

static void flow_include_x(long double x, long double * lo, long double * hi, bool * found) {
  if (!*found || x < *lo) *lo = x;
  if (!*found || x > *hi) *hi = x;
  *found = true;
}

/** Bounds of a polygon clipped to a horizontal slab. */
static bool flow_polygon_slab(const GFNT_FlowPoint * points, size_t count,
    long double y0, long double y1, long double * x0, long double * x1,
    long double * bottom) {
  bool found = false;
  long double lo = 0, hi = 0, min_y = points[0].y, max_y = points[0].y;
  for (size_t i = 1; i < count; i++) {
    if (points[i].y < min_y) min_y = points[i].y;
    if (points[i].y > max_y) max_y = points[i].y;
  }
  if (y1 <= min_y || y0 >= max_y) return false;
  for (size_t i = 0; i < count; i++) {
    const GFNT_FlowPoint a = points[i], b = points[(i + 1) % count];
    const long double ay = a.y, by = b.y;
    if (ay > max_y) max_y = ay;
    if (ay >= y0 && ay <= y1) flow_include_x(a.x, &lo, &hi, &found);
    if (ay != by) {
      const long double low = ay < by ? ay : by;
      const long double high = ay > by ? ay : by;
      const long double scans[] = {y0, y1};
      for (size_t k = 0; k < 2; k++) {
        const long double y = scans[k];
        if (y >= low && y <= high) {
          const long double x = a.x + (y - ay) * ((long double)b.x - a.x) / (by - ay);
          flow_include_x(x, &lo, &hi, &found);
        }
      }
    }
  }
  if (!found) return false;
  *x0 = lo; *x1 = hi; *bottom = max_y;
  return true;
}

/** Bounds of one exclusion over the line's cross-axis interval. */
static bool flow_exclusion_slab(const GFNT_Exclusion * item, GFNT_F26Dot6 top,
    GFNT_F26Dot6 height, long double * x0, long double * x1, int64_t * bottom) {
  const long double margin = item->margin;
  const long double y0 = (long double)top - margin;
  const long double y1 = (long double)top + height + margin;
  long double lo = 0, hi = 0, lower = 0;
  switch (item->shape) {
    case GFNT_EXCLUSION_RECTANGLE: {
      const GFNT_Rect r = item->geometry.rectangle;
      const long double upper = (long double)r.y - margin;
      lower = (long double)r.y + r.height + margin;
      if (y1 <= upper || y0 >= lower) return false;
      lo = (long double)r.x - margin;
      hi = (long double)r.x + r.width + margin;
      break;
    }
    case GFNT_EXCLUSION_CIRCLE: {
      const GFNT_FlowPoint center = item->geometry.circle.center;
      const long double radius = (long double)item->geometry.circle.radius + margin;
      const long double top_edge = (long double)center.y - radius;
      lower = (long double)center.y + radius;
      if (y1 <= top_edge || y0 >= lower) return false;
      long double dy = 0;
      if ((long double)center.y < y0) dy = y0 - center.y;
      else if ((long double)center.y > y1) dy = center.y - y1;
      const long double reach = sqrtl(radius * radius - dy * dy);
      lo = (long double)center.x - reach;
      hi = (long double)center.x + reach;
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
  lo = floorl(lo); hi = ceill(hi);
  if (lo < INT32_MIN || hi > INT32_MAX || lower < INT32_MIN || lower > INT32_MAX) return false;
  *x0 = lo; *x1 = hi; *bottom = (int64_t)lower;
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
    long double left, right;
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
        start = (int64_t)right;
        affects = true;
      }
    }
    else if (left < end) {
      end = (int64_t)left;
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
