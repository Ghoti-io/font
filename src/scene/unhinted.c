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
 * The one unhinted rounding (D1); see unhinted.h.
 *
 * **Why the order changed.** Until story 14b an unhinted glyph was scaled first
 * (::gfnt_outline_scale(), `p * S >> 16` with ties away from zero, y up) and
 * decomposed after, in pixels, so `glyf`'s implied midpoints were floored in
 * pixel 26.6. The general route of a scene does the opposite: it decomposes in
 * design units, maps, and rounds once by AD-6's `floor(x * 64 + 0.5)`, half up
 * in y down. The two disagreed on every tie (a 2048-unit face at 16 ppem has a
 * tie at every odd unit) and on every midpoint, so the fast tier could not equal
 * the general route under the snapped map (AD-5). Here the walk is in design
 * units and each emitted point is rounded once, by `shape-geometry`'s integer
 * spelling of AD-6 (::gshgeom_scale_26_6), so it does.
 *
 * Nothing here is floating point.
 */

#include <ghoti.io/font/macros.h>
#include "unhinted.h"

#include "../outline/outline.h"
#include "../sfnt/sfnt.h"

GFNT_Result gfnt_unhinted_geometry_error(GSHGEOM_Result result,
    GFNT_Error * error) {
  switch (result) {
    case GSHGEOM_OK:
      return GFNT_OK;
    case GSHGEOM_ERR_LIMIT:
      return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
          "a glyph's point or path is past a limit of the device path");
    case GSHGEOM_ERR_OOM:
      return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
          "no memory for a glyph's device path");
    default:
      return gfnt_error_set(error, GFNT_ERR_INTERNAL, 0, 0, GFNT_GLYPH_NONE,
          "the device path refused a segment of the outline");
  }
}

/** The walk's state: the rounding's parameters and where the points go. */
typedef struct GFNT_Unhinted {
  int32_t scale;
  int32_t fx;
  int32_t fy;
  const GFNT_DeviceSink * sink;
  void * user;
  GFNT_Error * error;
} GFNT_Unhinted;

/** One point by D1: `(fx + scale_26_6(x, S, 0), fy + scale_26_6(y, S, 1))`. */
static GFNT_Result gfnt_unhinted_point(const GFNT_Unhinted * walk,
    GFNT_Point point, GSHGEOM_Device_Point * out) {
  int32_t x = 0;
  int32_t y = 0;
  GSHGEOM_Result r = gshgeom_scale_26_6(point.x, walk->scale, 0, &x);
  int64_t ax;
  int64_t ay;

  if (r == GSHGEOM_OK) {
    r = gshgeom_scale_26_6(point.y, walk->scale, 1, &y);
  }
  if (r != GSHGEOM_OK) {
    return gfnt_unhinted_geometry_error(r, walk->error);
  }
  ax = (int64_t)walk->fx + x;
  ay = (int64_t)walk->fy + y;
  if (ax < INT32_MIN || ax > INT32_MAX || ay < INT32_MIN || ay > INT32_MAX) {
    return gfnt_unhinted_geometry_error(GSHGEOM_ERR_LIMIT, walk->error);
  }
  out->x = (int32_t)ax;
  out->y = (int32_t)ay;
  return GFNT_OK;
}

static GFNT_Result gfnt_unhinted_move(void * user, GFNT_Point to) {
  GFNT_Unhinted * walk = user;
  GSHGEOM_Device_Point p;
  GFNT_Result r = gfnt_unhinted_point(walk, to, &p);

  return r != GFNT_OK ? r : walk->sink->move_to(walk->user, p);
}

static GFNT_Result gfnt_unhinted_line(void * user, GFNT_Point to) {
  GFNT_Unhinted * walk = user;
  GSHGEOM_Device_Point p;
  GFNT_Result r = gfnt_unhinted_point(walk, to, &p);

  return r != GFNT_OK ? r : walk->sink->line_to(walk->user, p);
}

static GFNT_Result gfnt_unhinted_quad(void * user, GFNT_Point control,
    GFNT_Point to) {
  GFNT_Unhinted * walk = user;
  GSHGEOM_Device_Point c;
  GSHGEOM_Device_Point p;
  GFNT_Result r = gfnt_unhinted_point(walk, control, &c);

  if (r == GFNT_OK) {
    r = gfnt_unhinted_point(walk, to, &p);
  }
  return r != GFNT_OK ? r : walk->sink->quad_to(walk->user, c, p);
}

static GFNT_Result gfnt_unhinted_cubic(void * user, GFNT_Point c1,
    GFNT_Point c2, GFNT_Point to) {
  GFNT_Unhinted * walk = user;
  GSHGEOM_Device_Point a;
  GSHGEOM_Device_Point b;
  GSHGEOM_Device_Point p;
  GFNT_Result r = gfnt_unhinted_point(walk, c1, &a);

  if (r == GFNT_OK) {
    r = gfnt_unhinted_point(walk, c2, &b);
  }
  if (r == GFNT_OK) {
    r = gfnt_unhinted_point(walk, to, &p);
  }
  return r != GFNT_OK ? r : walk->sink->cubic_to(walk->user, a, b, p);
}

static GFNT_Result gfnt_unhinted_close(void * user) {
  GFNT_Unhinted * walk = user;

  return walk->sink->close(walk->user);
}

GFNT_Result gfnt_unhinted_walk(const GFNT_Outline * units, int32_t scale,
    int32_t fx, int32_t fy, const GFNT_DeviceSink * sink, void * user,
    GFNT_Error * error) {
  static const GFNT_OutlineSink adapter = {
    gfnt_unhinted_move,
    gfnt_unhinted_line,
    gfnt_unhinted_quad,
    gfnt_unhinted_cubic,
    gfnt_unhinted_close,
  };
  GFNT_Unhinted walk;

  if (!units || !sink) {
    return GFNT_ERR_INVALID;
  }
  if (gfnt_outline_space(units) != GFNT_OUTLINE_UNITS) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "the unhinted rounding takes an outline in design units");
  }
  walk.scale = scale;
  walk.fx = fx;
  walk.fy = fy;
  walk.sink = sink;
  walk.user = user;
  walk.error = error;
  return gfnt_outline_decompose(units, &adapter, &walk, error);
}

// --- into a device path (the provider) -------------------------------------

typedef struct GFNT_UnhintedPath {
  GSHGEOM_Device_Path * path;
  GFNT_Error * error;
} GFNT_UnhintedPath;

static GFNT_Result gfnt_path_move(void * user, GSHGEOM_Device_Point to) {
  GFNT_UnhintedPath * p = user;

  return gfnt_unhinted_geometry_error(gshgeom_device_path_move_to(p->path, to),
      p->error);
}

static GFNT_Result gfnt_path_line(void * user, GSHGEOM_Device_Point to) {
  GFNT_UnhintedPath * p = user;

  return gfnt_unhinted_geometry_error(gshgeom_device_path_line_to(p->path, to),
      p->error);
}

static GFNT_Result gfnt_path_quad(void * user, GSHGEOM_Device_Point c,
    GSHGEOM_Device_Point to) {
  GFNT_UnhintedPath * p = user;

  return gfnt_unhinted_geometry_error(
      gshgeom_device_path_quadratic_to(p->path, c, to), p->error);
}

static GFNT_Result gfnt_path_cubic(void * user, GSHGEOM_Device_Point a,
    GSHGEOM_Device_Point b, GSHGEOM_Device_Point to) {
  GFNT_UnhintedPath * p = user;

  return gfnt_unhinted_geometry_error(
      gshgeom_device_path_cubic_to(p->path, a, b, to), p->error);
}

static GFNT_Result gfnt_path_close(void * user) {
  GFNT_UnhintedPath * p = user;

  return gfnt_unhinted_geometry_error(gshgeom_device_path_close(p->path),
      p->error);
}

GFNT_Result gfnt_unhinted_device_path(const GFNT_Outline * units,
    int32_t scale, int32_t fx, int32_t fy, GSHGEOM_Device_Path * out,
    GFNT_Error * error) {
  static const GFNT_DeviceSink sink = {
    gfnt_path_move,
    gfnt_path_line,
    gfnt_path_quad,
    gfnt_path_cubic,
    gfnt_path_close,
  };
  GFNT_UnhintedPath p;

  if (!out) {
    return GFNT_ERR_INVALID;
  }
  p.path = out;
  p.error = error;
  return gfnt_unhinted_walk(units, scale, fx, fy, &sink, &p, error);
}

// --- into a pixel outline (the direct renderer) ----------------------------

typedef struct GFNT_UnhintedOutline {
  GFNT_Outline * outline;
  GFNT_Error * error;
} GFNT_UnhintedOutline;

/**
 * Add one device point as an outline point: y back up, so that the
 * rasteriser's one flip (raster.c) turns it down again unchanged.
 */
static GFNT_Result gfnt_pixels_add(GFNT_UnhintedOutline * o,
    GSHGEOM_Device_Point p, GFNT_PointTag tag) {
  GFNT_Point point;

  if (p.y == INT32_MIN) {
    return gfnt_unhinted_geometry_error(GSHGEOM_ERR_LIMIT, o->error);
  }
  point.x = p.x;
  point.y = -p.y;
  return gfnt_outline_add_point(o->outline, point, tag, o->error);
}

static GFNT_Result gfnt_pixels_move(void * user, GSHGEOM_Device_Point to) {
  GFNT_UnhintedOutline * o = user;
  GFNT_Result r = gfnt_outline_begin_contour(o->outline, o->error);

  return r != GFNT_OK ? r : gfnt_pixels_add(o, to, GFNT_POINT_ON);
}

static GFNT_Result gfnt_pixels_line(void * user, GSHGEOM_Device_Point to) {
  return gfnt_pixels_add(user, to, GFNT_POINT_ON);
}

static GFNT_Result gfnt_pixels_quad(void * user, GSHGEOM_Device_Point c,
    GSHGEOM_Device_Point to) {
  GFNT_Result r = gfnt_pixels_add(user, c, GFNT_POINT_QUAD);

  return r != GFNT_OK ? r : gfnt_pixels_add(user, to, GFNT_POINT_ON);
}

static GFNT_Result gfnt_pixels_cubic(void * user, GSHGEOM_Device_Point a,
    GSHGEOM_Device_Point b, GSHGEOM_Device_Point to) {
  GFNT_Result r = gfnt_pixels_add(user, a, GFNT_POINT_CUBIC);

  if (r == GFNT_OK) {
    r = gfnt_pixels_add(user, b, GFNT_POINT_CUBIC);
  }
  return r != GFNT_OK ? r : gfnt_pixels_add(user, to, GFNT_POINT_ON);
}

static GFNT_Result gfnt_pixels_close(void * user) {
  // The contour ends where its walk did: a curve back to the start was added
  // with its end point, and a straight closing segment is the close itself,
  // which the rasteriser's walk of this contour makes again.
  (void)user;
  return GFNT_OK;
}

GFNT_Result gfnt_unhinted_pixels(const GFNT_Outline * units, int32_t scale,
    const GFNT_Allocator * allocator, GFNT_Outline ** out, GFNT_Error * error) {
  static const GFNT_DeviceSink sink = {
    gfnt_pixels_move,
    gfnt_pixels_line,
    gfnt_pixels_quad,
    gfnt_pixels_cubic,
    gfnt_pixels_close,
  };
  GFNT_UnhintedOutline o;
  GFNT_Limits caps;
  GFNT_Result r;

  if (!units || !out) {
    return GFNT_ERR_INVALID;
  }
  o.outline = NULL;
  o.error = error;
  r = gfnt_outline_create(allocator, &o.outline, error);
  if (r != GFNT_OK) {
    return r;
  }
  // The walk adds a point for each implied midpoint, so the copy can hold more
  // points than the outline it came from, which its limits already bounded: the
  // copy's caps are not the font's to spend twice.
  caps = units->limits;
  caps.max_outline_points = SIZE_MAX;
  caps.max_contours = SIZE_MAX;
  gfnt_outline_set_limits(o.outline, &caps);
  r = gfnt_unhinted_walk(units, scale, 0, 0, &sink, &o, error);
  if (r != GFNT_OK) {
    gfnt_outline_destroy(o.outline);
    return r;
  }
  o.outline->space = GFNT_OUTLINE_PIXELS;
  *out = o.outline;
  return GFNT_OK;
}

GFNT_Result gfnt_unhinted_render(const GFNT_Face * face, uint32_t glyph,
    uint16_t upem, uint32_t ppem, const GFNT_RasterOptions * options,
    const GFNT_Allocator * allocator, GFNT_Coverage * out_coverage,
    GFNT_Error * error) {
  GFNT_Outline * units = NULL;
  GFNT_Outline * pixels = NULL;
  GFNT_Result result;

  result = gfnt_face_glyph_outline(face, glyph,
      options ? options->variation : NULL, allocator, &units, error);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_unhinted_pixels(units, gfnt_scale_for_ppem(upem, ppem),
      allocator, &pixels, error);
  gfnt_outline_destroy(units);
  if (result == GFNT_OK) {
    // The face's caps, not the defaults: a face loaded under a tight
    // max_raster_bytes must not have it ignored at the point it matters.
    result = gfnt_raster_outline(pixels, options, &face->limits, allocator,
        out_coverage, error);
  }
  gfnt_outline_destroy(pixels);
  return result;
}
