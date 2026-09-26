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
 * `GFNT_Outline`: storage, transforms, bounds, and the one place `glyf`'s
 * implicit points are resolved.
 *
 * documentation/design.md sections 7.3 and 8.1.
 */

#include <ghoti.io/font/macros.h>
#include <ghoti.io/cutil/safemath.h>
#include <ghoti.io/font/outline.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "../core/fixed.h"
#include "outline.h"

/** Smallest point allocation, so a glyph of a few points is one malloc. */
#define GFNT_OUTLINE_MIN_POINTS 32u
/** Smallest contour allocation. */
#define GFNT_OUTLINE_MIN_CONTOURS 4u
/**
 * How far a cubic is subdivided while bounding it.
 *
 * A quadratic's extremum is solved for exactly, so this only bounds cubics.
 * Each level halves the control polygon's deviation, so sixteen levels bring
 * any coordinate a font can express - 26.6 over an int16 range, 2^21 units -
 * below one 26.6 unit. The cap is what keeps a hostile curve from recursing
 * without bound; reaching it is not an error, because the control polygon
 * still contains the curve.
 */
#define GFNT_OUTLINE_BOUND_DEPTH 16

const char * gfnt_outline_space_string(GFNT_OutlineSpace space) {
  switch (space) {
    case GFNT_OUTLINE_UNITS:
      return "font units";
    case GFNT_OUTLINE_PIXELS:
      return "pixels";
    case GFNT_OUTLINE_SPACE_COUNT:
      break;
  }
  return "unknown space";
}

const char * gfnt_point_tag_string(GFNT_PointTag tag) {
  switch (tag) {
    case GFNT_POINT_ON:
      return "on";
    case GFNT_POINT_QUAD:
      return "quad";
    case GFNT_POINT_CUBIC:
      return "cubic";
    case GFNT_POINT_TAG_COUNT:
      break;
  }
  return "unknown tag";
}

bool gfnt_box_is_empty(const GFNT_Box * box) {
  if (!box) {
    return true;
  }
  return box->x_min > box->x_max || box->y_min > box->y_max;
}

GFNT_Point gfnt_outline_apply_matrix(GFNT_Point point, GFNT_F16Dot16 xx,
    GFNT_F16Dot16 xy, GFNT_F16Dot16 yx, GFNT_F16Dot16 yy) {
  GFNT_Point out;

  // One rounding per coordinate, on the sum, rather than one per product: a
  // component placed by two roundings and measured by one is a seam.
  out.x = gfnt_saturate32(gfnt_round_shift(
      gfnt_add_clamp64((int64_t)point.x * (int64_t)xx,
          (int64_t)point.y * (int64_t)xy),
      16));
  out.y = gfnt_saturate32(gfnt_round_shift(
      gfnt_add_clamp64((int64_t)point.x * (int64_t)yx,
          (int64_t)point.y * (int64_t)yy),
      16));
  return out;
}

void gfnt_outline_set_limits(GFNT_Outline * outline,
    const GFNT_Limits * limits) {
  if (!outline) {
    return;
  }
  if (limits) {
    outline->limits = *limits;
  }
  else {
    gfnt_limits_default(&outline->limits);
  }
}

GFNT_Result gfnt_outline_create(const GFNT_Allocator * allocator,
    GFNT_Outline ** out_outline, GFNT_Error * error) {
  GFNT_Outline * outline = NULL;

  gfnt_error_clear(error);
  if (!out_outline) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "gfnt_outline_create needs somewhere to put the outline");
  }
  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  outline = allocator->calloc_fn(allocator->ctx, 1, sizeof *outline);
  if (!outline) {
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for an outline");
  }
  outline->allocator = allocator;
  outline->space = GFNT_OUTLINE_UNITS;
  gfnt_limits_default(&outline->limits);
  *out_outline = outline;
  return GFNT_OK;
}

void gfnt_outline_destroy(GFNT_Outline * outline) {
  const GFNT_Allocator * allocator;

  if (!outline) {
    return;
  }
  allocator = outline->allocator;
  allocator->free_fn(allocator->ctx, outline->points);
  allocator->free_fn(allocator->ctx, outline->tags);
  allocator->free_fn(allocator->ctx, outline->contours);
  allocator->free_fn(allocator->ctx, outline);
}

void gfnt_outline_clear(GFNT_Outline * outline) {
  if (!outline) {
    return;
  }
  outline->point_count = 0;
  outline->contour_count = 0;
  outline->space = GFNT_OUTLINE_UNITS;
}

/**
 * The next doubling at or above @p wanted, starting from @p floor.
 */
static GFNT_Result gfnt_outline_next_capacity(size_t have, size_t wanted,
    size_t floor, size_t * out_capacity, GFNT_Error * error) {
  size_t next = have ? have : floor;

  while (next < wanted) {
    size_t doubled = 0;

    if (!gcu_safe_add_size(next, next, &doubled)) {
      return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
          "an outline array's size overflowed");
    }
    next = doubled;
  }
  *out_capacity = next;
  return GFNT_OK;
}

/**
 * Resize one array to @p capacity elements of @p element bytes.
 *
 * The new block comes back through @p out_array rather than being written to a
 * `void **` the caller cast a `GFNT_Point **` into: that cast is a
 * strict-aliasing violation, `make check-aliasing` is armed for exactly it, and
 * no sanitizer would have found it (`notes` and CONVENTIONS section 7 both say
 * the warning is the only instrument).
 */
static GFNT_Result gfnt_outline_resize(const GFNT_Allocator * allocator,
    void * array, size_t capacity, size_t element, void ** out_array,
    GFNT_Error * error) {
  size_t bytes = 0;
  void * grown = NULL;

  if (!gcu_safe_mul_size(capacity, element, &bytes)) {
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "an outline array's size overflowed");
  }
  grown = allocator->realloc_fn(allocator->ctx, array, bytes);
  if (!grown) {
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory to grow an outline");
  }
  *out_array = grown;
  return GFNT_OK;
}

GFNT_Result gfnt_outline_reserve(GFNT_Outline * outline, size_t points,
    size_t contours, GFNT_Error * error) {
  size_t wanted_points = 0;
  size_t wanted_contours = 0;
  GFNT_Result result;

  if (!outline) {
    return GFNT_ERR_INVALID;
  }
  if (!gcu_safe_add_size(outline->point_count, points, &wanted_points)
      || !gcu_safe_add_size(outline->contour_count, contours,
          &wanted_contours)) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
        "an outline's point or contour count overflowed");
  }
  // The cap is checked against what was asked for, not against the doubled
  // capacity: max_outline_points is a promise about the font.
  if (wanted_points > outline->limits.max_outline_points) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
        "more points in one glyph than max_outline_points allows");
  }
  if (wanted_contours > outline->limits.max_contours) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
        "more contours in one glyph than max_contours allows");
  }
  if (wanted_points > outline->point_capacity) {
    size_t capacity = 0;
    void * points = NULL;
    void * tags = NULL;

    result = gfnt_outline_next_capacity(outline->point_capacity, wanted_points,
        GFNT_OUTLINE_MIN_POINTS, &capacity, error);
    if (result != GFNT_OK) {
      return result;
    }
    // Both arrays are resized under one capacity, and the capacity is only
    // published once both have been. A `points` array longer than `tags`
    // would make a later add_point write a point and drop its tag, which
    // reads as a font whose curve lost a control point.
    result = gfnt_outline_resize(outline->allocator, outline->points, capacity,
        sizeof *outline->points, &points, error);
    if (result != GFNT_OK) {
      return result;
    }
    outline->points = points;
    result = gfnt_outline_resize(outline->allocator, outline->tags, capacity,
        sizeof *outline->tags, &tags, error);
    if (result != GFNT_OK) {
      // The points array kept its new block, which is larger than the
      // published capacity says. That is safe and the next reserve reuses it;
      // publishing the capacity here would claim room in `tags` that the
      // failure means it does not have.
      return result;
    }
    outline->tags = tags;
    outline->point_capacity = capacity;
  }
  if (wanted_contours > outline->contour_capacity) {
    size_t capacity = 0;
    void * contours = NULL;

    result = gfnt_outline_next_capacity(outline->contour_capacity,
        wanted_contours, GFNT_OUTLINE_MIN_CONTOURS, &capacity, error);
    if (result != GFNT_OK) {
      return result;
    }
    result = gfnt_outline_resize(outline->allocator, outline->contours,
        capacity, sizeof *outline->contours, &contours, error);
    if (result != GFNT_OK) {
      return result;
    }
    outline->contours = contours;
    outline->contour_capacity = capacity;
  }
  return GFNT_OK;
}

GFNT_Result gfnt_outline_begin_contour(GFNT_Outline * outline,
    GFNT_Error * error) {
  GFNT_Result result;

  if (!outline) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_outline_reserve(outline, 0, 1, error);
  if (result != GFNT_OK) {
    return result;
  }
  outline->contours[outline->contour_count] = outline->point_count;
  outline->contour_count += 1;
  return GFNT_OK;
}

GFNT_Result gfnt_outline_add_point(GFNT_Outline * outline, GFNT_Point point,
    GFNT_PointTag tag, GFNT_Error * error) {
  GFNT_Result result;

  if (!outline || tag >= GFNT_POINT_TAG_COUNT) {
    return GFNT_ERR_INVALID;
  }
  if (outline->contour_count == 0) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "a point was added before any contour was begun");
  }
  result = gfnt_outline_reserve(outline, 1, 0, error);
  if (result != GFNT_OK) {
    return result;
  }
  outline->points[outline->point_count] = point;
  outline->tags[outline->point_count] = (uint8_t)tag;
  outline->point_count += 1;
  outline->contours[outline->contour_count - 1] = outline->point_count;
  return GFNT_OK;
}

size_t gfnt_outline_point_count(const GFNT_Outline * outline) {
  return outline ? outline->point_count : 0;
}

size_t gfnt_outline_contour_count(const GFNT_Outline * outline) {
  return outline ? outline->contour_count : 0;
}

GFNT_OutlineSpace gfnt_outline_space(const GFNT_Outline * outline) {
  return outline ? outline->space : GFNT_OUTLINE_UNITS;
}

GFNT_Result gfnt_outline_point_at(const GFNT_Outline * outline, size_t index,
    GFNT_Point * out_point, GFNT_PointTag * out_tag) {
  if (!outline || index >= outline->point_count) {
    return GFNT_ERR_INVALID;
  }
  if (out_point) {
    *out_point = outline->points[index];
  }
  if (out_tag) {
    *out_tag = (GFNT_PointTag)outline->tags[index];
  }
  return GFNT_OK;
}

GFNT_Result gfnt_outline_contour_at(const GFNT_Outline * outline,
    size_t contour, size_t * out_first, size_t * out_count) {
  size_t first;
  size_t end;

  if (!outline || contour >= outline->contour_count) {
    return GFNT_ERR_INVALID;
  }
  first = contour ? outline->contours[contour - 1] : 0;
  end = outline->contours[contour];
  if (out_first) {
    *out_first = first;
  }
  if (out_count) {
    *out_count = end - first;
  }
  return GFNT_OK;
}

GFNT_Result gfnt_outline_append_transformed(GFNT_Outline * dest,
    const GFNT_Outline * source, GFNT_F16Dot16 xx, GFNT_F16Dot16 xy,
    GFNT_F16Dot16 yx, GFNT_F16Dot16 yy, GFNT_F26Dot6 dx, GFNT_F26Dot6 dy,
    GFNT_Error * error) {
  GFNT_Result result;
  size_t base;

  if (!dest || !source) {
    return GFNT_ERR_INVALID;
  }
  if (source->point_count == 0 && source->contour_count == 0) {
    return GFNT_OK;
  }
  result = gfnt_outline_reserve(dest, source->point_count,
      source->contour_count, error);
  if (result != GFNT_OK) {
    return result;
  }
  base = dest->point_count;
  for (size_t i = 0; i < source->point_count; ++i) {
    GFNT_Point moved = gfnt_outline_apply_matrix(source->points[i], xx, xy, yx,
        yy);

    moved.x = gfnt_saturate32((int64_t)moved.x + (int64_t)dx);
    moved.y = gfnt_saturate32((int64_t)moved.y + (int64_t)dy);
    dest->points[base + i] = moved;
    dest->tags[base + i] = source->tags[i];
  }
  for (size_t i = 0; i < source->contour_count; ++i) {
    dest->contours[dest->contour_count + i] = base + source->contours[i];
  }
  dest->point_count += source->point_count;
  dest->contour_count += source->contour_count;
  return GFNT_OK;
}

GFNT_Result gfnt_outline_transform(GFNT_Outline * outline, GFNT_F16Dot16 xx,
    GFNT_F16Dot16 xy, GFNT_F16Dot16 yx, GFNT_F16Dot16 yy, GFNT_F26Dot6 dx,
    GFNT_F26Dot6 dy) {
  if (!outline) {
    return GFNT_ERR_INVALID;
  }
  for (size_t i = 0; i < outline->point_count; ++i) {
    GFNT_Point moved = gfnt_outline_apply_matrix(outline->points[i], xx, xy, yx,
        yy);

    moved.x = gfnt_saturate32((int64_t)moved.x + (int64_t)dx);
    moved.y = gfnt_saturate32((int64_t)moved.y + (int64_t)dy);
    outline->points[i] = moved;
  }
  return GFNT_OK;
}

GFNT_Result gfnt_outline_translate(GFNT_Outline * outline, GFNT_F26Dot6 dx,
    GFNT_F26Dot6 dy) {
  if (!outline) {
    return GFNT_ERR_INVALID;
  }
  for (size_t i = 0; i < outline->point_count; ++i) {
    outline->points[i].x = gfnt_saturate32(
        (int64_t)outline->points[i].x + (int64_t)dx);
    outline->points[i].y = gfnt_saturate32(
        (int64_t)outline->points[i].y + (int64_t)dy);
  }
  return GFNT_OK;
}

GFNT_Result gfnt_outline_scale(GFNT_Outline * outline, GFNT_F16Dot16 scale,
    GFNT_Error * error) {
  gfnt_error_clear(error);
  if (!outline) {
    return GFNT_ERR_INVALID;
  }
  if (outline->space != GFNT_OUTLINE_UNITS) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "this outline is already in pixels and cannot be scaled twice");
  }
  for (size_t i = 0; i < outline->point_count; ++i) {
    // A 26.6 value times a 16.16 scale, rounded once: gfnt_f16dot16_mul is
    // that operation, whatever its name says about its usual operands.
    outline->points[i].x = gfnt_f16dot16_mul(outline->points[i].x, scale);
    outline->points[i].y = gfnt_f16dot16_mul(outline->points[i].y, scale);
  }
  outline->space = GFNT_OUTLINE_PIXELS;
  return GFNT_OK;
}

/** The point halfway between two, rounding towards negative infinity. */
static GFNT_Point gfnt_outline_midpoint(GFNT_Point a, GFNT_Point b) {
  GFNT_Point mid;

  // Floor rather than round-half-away-from-zero, and the choice is visible:
  // this is the on-curve point `glyf` leaves implicit, so a reference
  // implementation's rule is the one that has to be matched. FreeType and
  // fontTools both take the arithmetic mean with C's truncating shift on a
  // sum that is even as often as not; floor and truncate differ only for an
  // odd negative sum, and floor is what a half-open pixel grid wants.
  mid.x = (GFNT_F26Dot6)gfnt_floor_div((int64_t)a.x + (int64_t)b.x, 2);
  mid.y = (GFNT_F26Dot6)gfnt_floor_div((int64_t)a.y + (int64_t)b.y, 2);
  return mid;
}

/**
 * State of one contour's walk, so that the emit helpers stay short.
 */
typedef struct GFNT_Walk {
  const GFNT_OutlineSink * sink;
  void * user;
  GFNT_Point control[2];
  size_t pending;
  GFNT_PointTag pending_tag;
} GFNT_Walk;

/**
 * Emit whatever segment the pending control points and @p to make.
 */
static GFNT_Result gfnt_outline_flush(GFNT_Walk * walk, GFNT_Point to,
    GFNT_Error * error) {
  GFNT_Result result;

  if (walk->pending == 0) {
    if (!walk->sink->line_to) {
      return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, 0, 0, GFNT_GLYPH_NONE,
          "this outline has a straight segment and the sink has no line_to");
    }
    return walk->sink->line_to(walk->user, to);
  }
  if (walk->pending_tag == GFNT_POINT_QUAD) {
    if (walk->pending != 1) {
      return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
          "a quadratic segment has more than one control point");
    }
    if (!walk->sink->quad_to) {
      return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, 0, 0, GFNT_GLYPH_NONE,
          "this outline has a quadratic and the sink has no quad_to");
    }
    result = walk->sink->quad_to(walk->user, walk->control[0], to);
    walk->pending = 0;
    return result;
  }
  if (walk->pending != 2) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "a cubic segment has one control point rather than two");
  }
  if (!walk->sink->cubic_to) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, 0, 0, GFNT_GLYPH_NONE,
        "this outline has a cubic and the sink has no cubic_to");
  }
  result = walk->sink->cubic_to(walk->user, walk->control[0], walk->control[1],
      to);
  walk->pending = 0;
  return result;
}

/**
 * Take one of the contour's points into the walk.
 */
static GFNT_Result gfnt_outline_step(GFNT_Walk * walk, GFNT_Point point,
    GFNT_PointTag tag, GFNT_Error * error) {
  if (tag == GFNT_POINT_ON) {
    return gfnt_outline_flush(walk, point, error);
  }
  if (walk->pending != 0 && walk->pending_tag != tag) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "a quadratic and a cubic control point are adjacent");
  }
  if (tag == GFNT_POINT_QUAD) {
    if (walk->pending == 1) {
      // Two off-curve points in a row: the on-curve point between them is
      // implicit and is their midpoint. This is the rule glyf leaves to the
      // reader, and the only place it is applied.
      GFNT_Point implied = gfnt_outline_midpoint(walk->control[0], point);
      GFNT_Result result = gfnt_outline_flush(walk, implied, error);

      if (result != GFNT_OK) {
        return result;
      }
    }
    walk->control[0] = point;
    walk->pending = 1;
    walk->pending_tag = GFNT_POINT_QUAD;
    return GFNT_OK;
  }
  if (walk->pending >= 2) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "three cubic control points in a row, which no curve has");
  }
  walk->control[walk->pending] = point;
  walk->pending += 1;
  walk->pending_tag = GFNT_POINT_CUBIC;
  return GFNT_OK;
}

GFNT_Result gfnt_outline_decompose(const GFNT_Outline * outline,
    const GFNT_OutlineSink * sink, void * user, GFNT_Error * error) {
  gfnt_error_clear(error);
  if (!outline || !sink || !sink->move_to || !sink->close) {
    return GFNT_ERR_INVALID;
  }

  for (size_t contour = 0; contour < outline->contour_count; ++contour) {
    size_t first = contour ? outline->contours[contour - 1] : 0;
    size_t end = outline->contours[contour];
    size_t count = end - first;
    size_t last = end - 1;
    GFNT_Point start;
    size_t begin;
    size_t steps;
    GFNT_Walk walk;
    GFNT_Result result;

    if (count == 0) {
      // An empty contour draws nothing. glyf cannot express one; a caller
      // building an outline by hand can, and a move_to with no points is worse
      // than silence.
      continue;
    }
    walk.sink = sink;
    walk.user = user;
    walk.pending = 0;
    walk.pending_tag = GFNT_POINT_ON;

    if (outline->tags[first] == GFNT_POINT_ON) {
      start = outline->points[first];
      begin = first + 1;
      steps = count - 1;
    }
    else if (outline->tags[last] == GFNT_POINT_ON) {
      // The contour begins off-curve: it starts at its own last point, and
      // that point is therefore not walked again.
      start = outline->points[last];
      begin = first;
      steps = count - 1;
    }
    else {
      // Every point is off-curve. The start is the implied midpoint between
      // the last and the first, and all of them are walked.
      start = gfnt_outline_midpoint(outline->points[last],
          outline->points[first]);
      begin = first;
      steps = count;
    }

    result = sink->move_to(user, start);
    if (result != GFNT_OK) {
      return result;
    }
    for (size_t step = 0; step < steps; ++step) {
      size_t index = first + ((begin - first) + step) % count;

      result = gfnt_outline_step(&walk, outline->points[index],
          (GFNT_PointTag)outline->tags[index], error);
      if (result != GFNT_OK) {
        return result;
      }
    }
    if (walk.pending != 0) {
      // A curve that runs into the contour's start has to be drawn; a straight
      // closing segment is what close() means.
      result = gfnt_outline_flush(&walk, start, error);
      if (result != GFNT_OK) {
        return result;
      }
    }
    result = sink->close(user);
    if (result != GFNT_OK) {
      return result;
    }
  }
  return GFNT_OK;
}

/**
 * A box being accumulated by the bounds walk.
 */
typedef struct GFNT_Bounder {
  GFNT_Box box;
  GFNT_Point current;
} GFNT_Bounder;

static void gfnt_bounder_add(GFNT_Bounder * bounder, GFNT_Point point) {
  if (point.x < bounder->box.x_min) {
    bounder->box.x_min = point.x;
  }
  if (point.x > bounder->box.x_max) {
    bounder->box.x_max = point.x;
  }
  if (point.y < bounder->box.y_min) {
    bounder->box.y_min = point.y;
  }
  if (point.y > bounder->box.y_max) {
    bounder->box.y_max = point.y;
  }
}

/**
 * Widen the box by a quadratic's extremum on one axis, exactly.
 *
 * With `den = p0 - 2*p1 + p2` and `t = (p0 - p1) / den`, the extremum's value
 * is `p0 - (p1 - p0)^2 / den` - an identity, so no curve is sampled and no
 * tolerance enters. The quotient is taken away from `p0` so that the box
 * contains the exact value rather than a rounded one: floor when the extremum
 * is below `p0`, ceiling when above.
 */
static void gfnt_bound_quadratic(GFNT_F26Dot6 p0, GFNT_F26Dot6 p1,
    GFNT_F26Dot6 p2, GFNT_F26Dot6 * out_min, GFNT_F26Dot6 * out_max) {
  int64_t den = (int64_t)p0 - 2 * (int64_t)p1 + (int64_t)p2;
  int64_t num = (int64_t)p0 - (int64_t)p1;
  int64_t delta = (int64_t)p1 - (int64_t)p0;
  int64_t square = delta * delta;
  int64_t value;

  if (den == 0) {
    // The curve is monotone on this axis: its endpoints already bound it.
    return;
  }
  // t must lie strictly inside the segment, which is num/den in (0, 1).
  if ((num < 0) != (den < 0) || num == 0) {
    return;
  }
  if ((num < 0 ? -num : num) >= (den < 0 ? -den : den)) {
    return;
  }
  if (den > 0) {
    value = (int64_t)p0 - gfnt_ceil_div(square, den);
    if (value < *out_min) {
      *out_min = gfnt_saturate32(value);
    }
  }
  else {
    value = (int64_t)p0 + gfnt_ceil_div(square, -den);
    if (value > *out_max) {
      *out_max = gfnt_saturate32(value);
    }
  }
}

/**
 * Widen the box by a cubic, by subdividing until the control polygon is flat.
 */
static void gfnt_bound_cubic(GFNT_Bounder * bounder, GFNT_Point p0,
    GFNT_Point p1, GFNT_Point p2, GFNT_Point p3, int depth) {
  GFNT_Point a;
  GFNT_Point b;
  GFNT_Point c;
  GFNT_Point d;
  GFNT_Point e;
  GFNT_Point f;
  bool flat_x;
  bool flat_y;

  gfnt_bounder_add(bounder, p3);
  flat_x = (p1.x >= p0.x && p1.x <= p3.x && p2.x >= p0.x && p2.x <= p3.x)
      || (p1.x <= p0.x && p1.x >= p3.x && p2.x <= p0.x && p2.x >= p3.x);
  flat_y = (p1.y >= p0.y && p1.y <= p3.y && p2.y >= p0.y && p2.y <= p3.y)
      || (p1.y <= p0.y && p1.y >= p3.y && p2.y <= p0.y && p2.y >= p3.y);
  if ((flat_x && flat_y) || depth <= 0) {
    // Monotone on both axes, or out of budget: the endpoints bound it, and the
    // control points bound it in the worst case.
    if (!(flat_x && flat_y)) {
      gfnt_bounder_add(bounder, p1);
      gfnt_bounder_add(bounder, p2);
    }
    return;
  }
  a = gfnt_outline_midpoint(p0, p1);
  b = gfnt_outline_midpoint(p1, p2);
  c = gfnt_outline_midpoint(p2, p3);
  d = gfnt_outline_midpoint(a, b);
  e = gfnt_outline_midpoint(b, c);
  f = gfnt_outline_midpoint(d, e);
  gfnt_bound_cubic(bounder, p0, a, d, f, depth - 1);
  gfnt_bound_cubic(bounder, f, e, c, p3, depth - 1);
}

static GFNT_Result gfnt_bound_move(void * user, GFNT_Point to) {
  GFNT_Bounder * bounder = user;

  gfnt_bounder_add(bounder, to);
  bounder->current = to;
  return GFNT_OK;
}

static GFNT_Result gfnt_bound_line(void * user, GFNT_Point to) {
  GFNT_Bounder * bounder = user;

  gfnt_bounder_add(bounder, to);
  bounder->current = to;
  return GFNT_OK;
}

static GFNT_Result gfnt_bound_quad(void * user, GFNT_Point control,
    GFNT_Point to) {
  GFNT_Bounder * bounder = user;

  gfnt_bounder_add(bounder, to);
  gfnt_bound_quadratic(bounder->current.x, control.x, to.x,
      &bounder->box.x_min, &bounder->box.x_max);
  gfnt_bound_quadratic(bounder->current.y, control.y, to.y,
      &bounder->box.y_min, &bounder->box.y_max);
  bounder->current = to;
  return GFNT_OK;
}

static GFNT_Result gfnt_bound_cubic_segment(void * user, GFNT_Point c1,
    GFNT_Point c2, GFNT_Point to) {
  GFNT_Bounder * bounder = user;

  gfnt_bound_cubic(bounder, bounder->current, c1, c2, to,
      GFNT_OUTLINE_BOUND_DEPTH);
  bounder->current = to;
  return GFNT_OK;
}

static GFNT_Result gfnt_bound_close(void * user) {
  (void)user;
  return GFNT_OK;
}

/** An empty box, which every widening replaces. */
static void gfnt_box_clear(GFNT_Box * box) {
  box->x_min = INT32_MAX;
  box->y_min = INT32_MAX;
  box->x_max = INT32_MIN;
  box->y_max = INT32_MIN;
}

GFNT_Result gfnt_outline_bounds(const GFNT_Outline * outline,
    GFNT_Box * out_box) {
  static const GFNT_OutlineSink sink = {
    gfnt_bound_move,
    gfnt_bound_line,
    gfnt_bound_quad,
    gfnt_bound_cubic_segment,
    gfnt_bound_close,
  };
  GFNT_Bounder bounder;
  GFNT_Result result;

  if (!outline || !out_box) {
    return GFNT_ERR_INVALID;
  }
  gfnt_box_clear(&bounder.box);
  bounder.current.x = 0;
  bounder.current.y = 0;
  if (outline->point_count == 0) {
    *out_box = bounder.box;
    return GFNT_OK;
  }
  result = gfnt_outline_decompose(outline, &sink, &bounder, NULL);
  if (result != GFNT_OK) {
    return result;
  }
  *out_box = bounder.box;
  return GFNT_OK;
}

GFNT_Result gfnt_outline_control_box(const GFNT_Outline * outline,
    GFNT_Box * out_box) {
  GFNT_Bounder bounder;

  if (!outline || !out_box) {
    return GFNT_ERR_INVALID;
  }
  gfnt_box_clear(&bounder.box);
  for (size_t i = 0; i < outline->point_count; ++i) {
    gfnt_bounder_add(&bounder, outline->points[i]);
  }
  *out_box = bounder.box;
  return GFNT_OK;
}

GFNT_Result gfnt_outline_dump(const GFNT_Outline * outline, FILE * out) {
  GFNT_Box box;

  if (!outline || !out) {
    return GFNT_ERR_INVALID;
  }
  if (outline->point_count == 0) {
    // Said rather than left out: a differential reads nothing as agreement.
    if (fprintf(out, "outline: no contours\n") < 0) {
      return GFNT_ERR_IO;
    }
    return GFNT_OK;
  }
  if (fprintf(out, "outline: space %s, points %zu, contours %zu\n",
          gfnt_outline_space_string(outline->space), outline->point_count,
          outline->contour_count)
      < 0) {
    return GFNT_ERR_IO;
  }
  for (size_t contour = 0; contour < outline->contour_count; ++contour) {
    size_t first = 0;
    size_t count = 0;

    (void)gfnt_outline_contour_at(outline, contour, &first, &count);
    if (fprintf(out, "outline contour %zu: first %zu, points %zu\n", contour,
            first, count)
        < 0) {
      return GFNT_ERR_IO;
    }
  }
  for (size_t i = 0; i < outline->point_count; ++i) {
    if (fprintf(out, "outline point %zu: %d %d %s\n", i, outline->points[i].x,
            outline->points[i].y,
            gfnt_point_tag_string((GFNT_PointTag)outline->tags[i]))
        < 0) {
      return GFNT_ERR_IO;
    }
  }
  if (gfnt_outline_bounds(outline, &box) == GFNT_OK
      && !gfnt_box_is_empty(&box)) {
    if (fprintf(out, "outline bounds: %d %d %d %d\n", box.x_min, box.y_min,
            box.x_max, box.y_max)
        < 0) {
      return GFNT_ERR_IO;
    }
  }
  return GFNT_OK;
}

/**
 * The path dump's sink: one line per segment, and the stream to write to.
 */
typedef struct GFNT_PathDump {
  FILE * out;
  GFNT_Result result;
} GFNT_PathDump;

static GFNT_Result gfnt_path_line(GFNT_PathDump * dump, const char * verb,
    const GFNT_Point * points, size_t count) {
  if (fprintf(dump->out, "path %s", verb) < 0) {
    dump->result = GFNT_ERR_IO;
    return GFNT_ERR_IO;
  }
  for (size_t i = 0; i < count; ++i) {
    if (fprintf(dump->out, " %d %d", points[i].x, points[i].y) < 0) {
      dump->result = GFNT_ERR_IO;
      return GFNT_ERR_IO;
    }
  }
  if (fprintf(dump->out, "\n") < 0) {
    dump->result = GFNT_ERR_IO;
    return GFNT_ERR_IO;
  }
  return GFNT_OK;
}

static GFNT_Result gfnt_path_move(void * user, GFNT_Point to) {
  return gfnt_path_line(user, "move", &to, 1);
}

static GFNT_Result gfnt_path_line_to(void * user, GFNT_Point to) {
  return gfnt_path_line(user, "line", &to, 1);
}

static GFNT_Result gfnt_path_quad(void * user, GFNT_Point control,
    GFNT_Point to) {
  GFNT_Point pair[2] = { control, to };

  return gfnt_path_line(user, "quad", pair, 2);
}

static GFNT_Result gfnt_path_cubic(void * user, GFNT_Point c1, GFNT_Point c2,
    GFNT_Point to) {
  GFNT_Point triple[3] = { c1, c2, to };

  return gfnt_path_line(user, "cubic", triple, 3);
}

static GFNT_Result gfnt_path_close(void * user) {
  return gfnt_path_line(user, "close", NULL, 0);
}

GFNT_Result gfnt_outline_path_dump(const GFNT_Outline * outline, FILE * out) {
  static const GFNT_OutlineSink sink = {
    gfnt_path_move,
    gfnt_path_line_to,
    gfnt_path_quad,
    gfnt_path_cubic,
    gfnt_path_close,
  };
  GFNT_PathDump dump;

  if (!outline || !out) {
    return GFNT_ERR_INVALID;
  }
  dump.out = out;
  dump.result = GFNT_OK;
  if (outline->point_count == 0) {
    if (fprintf(out, "path: empty\n") < 0) {
      return GFNT_ERR_IO;
    }
    return GFNT_OK;
  }
  return gfnt_outline_decompose(outline, &sink, &dump, NULL);
}
