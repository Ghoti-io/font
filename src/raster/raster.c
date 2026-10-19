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
 * The rasteriser's glue: an outline becomes a `shape-geometry` device path, the
 * path becomes coverage in `shape-cpu`, and the coverage becomes a
 * ::GFNT_Coverage.
 *
 * documentation/design.md section 8.1. The scan converter itself - cells, the
 * sweep, the fill rule, the trim - was this file's until story 9 of the shape
 * family's first milestone and now lives in `shape-cpu` (AD-1, AD-12), moved
 * without changing a pixel: `make golden` reproduces the committed renderings
 * byte for byte. What stays here is what only a font can say.
 *
 * **The flip.** An outline is y-up (design.md section 5.5); device space and
 * `shape-cpu`'s coverage are y-down. The flip lives in this file's frame
 * functions - ::gfnt_raster_place() turns a glyph-space point into a bitmap-space
 * one and ::gfnt_raster_view() turns `shape-cpu`'s y-down `left` and `top` back
 * into the y-up ones of a ::GFNT_Coverage - and it happens **before** the
 * curves are flattened, because the flattener's half-up midpoint is not
 * symmetric under negation: flattening first and flipping after would change
 * vertices, and with them pixels.
 *
 * **The flattener** is still `flatten.c` here, because ::GFNT_RasterOptions
 * takes a tolerance and `shape-geometry`'s flattener has one value (AD-12). It
 * produces the polyline `shape-geometry`'s does at the default tolerance, bit
 * for bit (story 7a's vectors), and the device path handed on holds lines.
 *
 * Nothing here is floating point, and nothing here reads the host's byte order.
 */

#include <ghoti.io/font/macros.h>
#include <ghoti.io/cutil/safemath.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/raster.h>
#include <ghoti.io/shape-cpu/coverage.h>
#include <ghoti.io/shape-geometry/device.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "../core/fixed.h"
#include "../outline/outline.h"
#include "../sfnt/sfnt.h"
#include "flatten.h"

/** One pixel in 26.6. */
#define GFNT_ONE_PIXEL 64

/** Default flattening tolerance: a sixteenth of a pixel. */
#define GFNT_RASTER_TOLERANCE (GFNT_ONE_PIXEL / 16)

/**
 * The rasteriser's working state while an outline is walked into a device path.
 */
typedef struct GFNT_Raster {
  const GFNT_Allocator * allocator;
  GFNT_Error * error;
  GFNT_Result result;        ///< The first failure, if any.
  GSHGEOM_Device_Path * path;///< The device path being built; lines and closes.

  int32_t left;              ///< Pixel column the bitmap starts at.
  int32_t top;               ///< Pixel row the bitmap starts at, y-up.
  uint32_t width;
  uint32_t height;

  GFNT_F26Dot6 tolerance;
  GFNT_F26Dot6 origin_x;     ///< The sub-pixel offset the glyph is drawn at.
  GFNT_F26Dot6 origin_y;
  GFNT_Point current;        ///< Where the pen is, 26.6, bitmap-relative.
  GFNT_Point contour_start;  ///< Where this contour began.
} GFNT_Raster;

const char * gfnt_fill_rule_string(GFNT_FillRule rule) {
  return gshgeom_fill_rule_string(rule);
}

void gfnt_coverage_destroy(GFNT_Coverage * coverage) {
  if (!coverage) {
    return;
  }
  if (coverage->data && coverage->allocator) {
    coverage->allocator->free_fn(coverage->allocator->ctx, coverage->data);
  }
  memset(coverage, 0, sizeof *coverage);
}

uint8_t gfnt_coverage_at(const GFNT_Coverage * coverage, uint32_t column,
    uint32_t row) {
  if (!coverage || !coverage->data || column >= coverage->width
      || row >= coverage->height) {
    return 0;
  }
  return coverage->data[(size_t)row * coverage->stride + column];
}

uint64_t gfnt_coverage_total(const GFNT_Coverage * coverage) {
  uint64_t total = 0;

  if (!coverage || !coverage->data) {
    return 0;
  }
  for (uint32_t row = 0; row < coverage->height; ++row) {
    const uint8_t * line = coverage->data + (size_t)row * coverage->stride;

    for (uint32_t column = 0; column < coverage->width; ++column) {
      total += line[column];
    }
  }
  return total;
}

GFNT_Result gfnt_coverage_apply_table(GFNT_Coverage * coverage,
    const uint8_t * table) {
  if (!coverage || !table) {
    return GFNT_ERR_INVALID;
  }
  for (uint32_t row = 0; row < coverage->height; ++row) {
    uint8_t * line = coverage->data + (size_t)row * coverage->stride;

    for (uint32_t column = 0; column < coverage->width; ++column) {
      line[column] = table[line[column]];
    }
  }
  return GFNT_OK;
}

/** FNV-1a's offset basis and prime, spelled rather than remembered. */
#define GFNT_FNV_BASIS 0xCBF29CE484222325ull
#define GFNT_FNV_PRIME 0x00000100000001B3ull

static uint64_t gfnt_fnv_byte(uint64_t hash, uint8_t byte) {
  return (hash ^ byte) * GFNT_FNV_PRIME;
}

/**
 * Mix a 32-bit field in least-significant-byte-first order.
 *
 * Written out rather than hashing the bytes of the variable, so that the hash
 * of a rendering is the same number on a big-endian machine - which is the
 * whole point of the gate that compares them.
 */
static uint64_t gfnt_fnv_u32(uint64_t hash, uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    hash = gfnt_fnv_byte(hash, (uint8_t)((value >> shift) & 0xFFu));
  }
  return hash;
}

uint64_t gfnt_coverage_hash(const GFNT_Coverage * coverage) {
  uint64_t hash = GFNT_FNV_BASIS;

  if (!coverage) {
    return hash;
  }
  hash = gfnt_fnv_u32(hash, coverage->width);
  hash = gfnt_fnv_u32(hash, coverage->height);
  hash = gfnt_fnv_u32(hash, (uint32_t)coverage->left);
  hash = gfnt_fnv_u32(hash, (uint32_t)coverage->top);
  hash = gfnt_fnv_u32(hash, (uint32_t)coverage->origin_x);
  hash = gfnt_fnv_u32(hash, (uint32_t)coverage->origin_y);
  hash = gfnt_fnv_u32(hash, (uint32_t)coverage->fill);
  // The rows and not the buffer: the stride is an allocation detail and a
  // rendering that differed only in its padding is the same rendering.
  for (uint32_t row = 0; row < coverage->height; ++row) {
    const uint8_t * line = coverage->data + (size_t)row * coverage->stride;

    for (uint32_t column = 0; column < coverage->width; ++column) {
      hash = gfnt_fnv_byte(hash, line[column]);
    }
  }
  return hash;
}

GFNT_Result gfnt_coverage_dump(const GFNT_Coverage * coverage, FILE * out) {
  if (!coverage || !out) {
    return GFNT_ERR_INVALID;
  }
  if (fprintf(out,
          "coverage: %ux%u at %d,%d, origin %d/%d, %s, total %llu, "
          "hash 0x%016llX\n",
          coverage->width, coverage->height, coverage->left, coverage->top,
          coverage->origin_x, coverage->origin_y,
          gfnt_fill_rule_string(coverage->fill),
          (unsigned long long)gfnt_coverage_total(coverage),
          (unsigned long long)gfnt_coverage_hash(coverage))
      < 0) {
    return GFNT_ERR_IO;
  }
  return GFNT_OK;
}

GFNT_Result gfnt_coverage_dump_art(const GFNT_Coverage * coverage,
    FILE * out) {
  static const char levels[] = ".:-=+*#%@";

  if (!coverage || !out) {
    return GFNT_ERR_INVALID;
  }
  if (coverage->height == 0 || coverage->width == 0) {
    if (fprintf(out, "coverage art: nothing to draw\n") < 0) {
      return GFNT_ERR_IO;
    }
    return GFNT_OK;
  }
  for (uint32_t row = 0; row < coverage->height; ++row) {
    for (uint32_t column = 0; column < coverage->width; ++column) {
      const uint8_t value = gfnt_coverage_at(coverage, column, row);
      // Nine levels over 0..255, and 255 alone gets the last one, so that a
      // pixel one short of full does not look full.
      const size_t index = value == 255u ? sizeof levels - 2u
                                         : (size_t)value * 8u / 255u;

      if (fputc(levels[index], out) == EOF) {
        return GFNT_ERR_IO;
      }
    }
    if (fputc('\n', out) == EOF) {
      return GFNT_ERR_IO;
    }
  }
  return GFNT_OK;
}

// --- the frame -------------------------------------------------------------

/**
 * Turn a glyph-space point into a bitmap-space one.
 *
 * The one place a point's y is flipped: the outline is y-up (design.md section
 * 5.5) and row 0 of the coverage is its top. Written as a subtraction from the
 * top edge rather than as a negation plus an offset, because the two differ by
 * a pixel and only one of them puts the top row at row zero.
 */
static GFNT_Point gfnt_raster_place(const GFNT_Raster * raster,
    GFNT_Point point) {
  GFNT_Point out;

  // The sub-pixel origin is applied here, to the geometry, and the bitmap's
  // extent was computed from the same shifted box. Shifting only the extent
  // moves the bitmap and leaves the glyph where it was - which the trim then
  // hides, because the column the shift added has nothing in it.
  out.x = point.x + raster->origin_x - raster->left * GFNT_ONE_PIXEL;
  out.y = (raster->top * GFNT_ONE_PIXEL) - (point.y + raster->origin_y);
  return out;
}

/**
 * Make a ::GFNT_Coverage of the coverage `shape-cpu` computed, taking its
 * pixels.
 *
 * The inverse of ::gfnt_raster_place() for the corner: `shape-cpu`'s `left` and
 * `top` are in the bitmap's frame (the rectangle it was asked for starts at
 * 0,0, y down), so the glyph-space column is `raster->left + left` and the
 * glyph-space upper edge of the top row, y-up, is `raster->top - top`. A
 * coverage with no pixel left is 0 by 0 with `left` and `top` zero, whatever
 * the rectangle was. @p from is left zeroed: its pixels are @p out's now.
 */
static void gfnt_raster_view(const GFNT_Raster * raster, GSHCPU_Coverage * from,
    GFNT_Coverage * out) {
  out->data = from->data;
  out->stride = from->stride;
  out->width = from->width;
  out->height = from->height;
  if (from->data) {
    out->left = raster->left + from->left;
    out->top = raster->top - from->top;
  }
  else {
    out->left = 0;
    out->top = 0;
  }
  memset(from, 0, sizeof *from);
}

// --- the sink the outline walk feeds ---------------------------------------

/** Remember the first failure of a device path call. */
static void gfnt_raster_check(GFNT_Raster * raster, GSHGEOM_Result result) {
  if (result == GSHGEOM_OK || raster->result != GFNT_OK) {
    return;
  }
  switch (result) {
    case GSHGEOM_ERR_LIMIT:
      raster->result = gfnt_error_set(raster->error, GFNT_ERR_LIMIT, 0, 0,
          GFNT_GLYPH_NONE, "this glyph's path is larger than a limit allows");
      break;
    case GSHGEOM_ERR_OOM:
      raster->result = gfnt_error_set(raster->error, GFNT_ERR_OOM, 0, 0,
          GFNT_GLYPH_NONE, "no memory for a glyph's device path");
      break;
    default:
      raster->result = gfnt_error_set(raster->error, GFNT_ERR_INTERNAL, 0, 0,
          GFNT_GLYPH_NONE, "the device path refused a segment of the outline");
      break;
  }
}

/**
 * Take one vertex of a flattened curve into the device path.
 *
 * The flattener itself is flatten.c, and its guarantee is proved there: the
 * polyline stays within the tolerance of the curve.
 */
static void gfnt_raster_emit(void * user, GFNT_Point to, uint32_t leaf,
    int level) {
  GFNT_Raster * raster = user;
  GSHGEOM_Device_Point point;

  (void)leaf;
  (void)level;
  point.x = to.x;
  point.y = to.y;
  raster->current = to;
  gfnt_raster_check(raster, gshgeom_device_path_line_to(raster->path, point));
}

static GFNT_Result gfnt_raster_move(void * user, GFNT_Point to) {
  GFNT_Raster * raster = user;
  const GFNT_Point placed = gfnt_raster_place(raster, to);
  GSHGEOM_Device_Point point;

  // A contour that has not been closed by the walk cannot happen -
  // gfnt_outline_decompose() closes every one - but the close is what makes
  // the winding number well defined, so the start is remembered here and used
  // there rather than assumed.
  raster->contour_start = placed;
  raster->current = placed;
  point.x = placed.x;
  point.y = placed.y;
  gfnt_raster_check(raster, gshgeom_device_path_move_to(raster->path, point));
  return raster->result;
}

static GFNT_Result gfnt_raster_line_to(void * user, GFNT_Point to) {
  GFNT_Raster * raster = user;
  const GFNT_Point placed = gfnt_raster_place(raster, to);

  gfnt_raster_emit(raster, placed, 0, 0);
  return raster->result;
}

static GFNT_Result gfnt_raster_quad_to(void * user, GFNT_Point control,
    GFNT_Point to) {
  GFNT_Raster * raster = user;

  // The cap cannot force a piece (flatten.c proves it for every tolerance this
  // file accepts), so the count it returns is not a failure to report.
  (void)gfnt_flatten_quad(raster->tolerance, raster->current,
      gfnt_raster_place(raster, control), gfnt_raster_place(raster, to),
      gfnt_raster_emit, raster);
  return raster->result;
}

static GFNT_Result gfnt_raster_cubic_to(void * user, GFNT_Point c1,
    GFNT_Point c2, GFNT_Point to) {
  GFNT_Raster * raster = user;

  (void)gfnt_flatten_cubic(raster->tolerance, raster->current,
      gfnt_raster_place(raster, c1), gfnt_raster_place(raster, c2),
      gfnt_raster_place(raster, to), gfnt_raster_emit, raster);
  return raster->result;
}

static GFNT_Result gfnt_raster_close(void * user) {
  GFNT_Raster * raster = user;

  // The closing line is shape-cpu's: a closed subpath of the device path ends
  // with a line back to its start.
  raster->current = raster->contour_start;
  gfnt_raster_check(raster, gshgeom_device_path_close(raster->path));
  return raster->result;
}

// --- the entry points ------------------------------------------------------

GFNT_Result gfnt_raster_outline(const GFNT_Outline * outline,
    const GFNT_RasterOptions * options, const GFNT_Limits * limits,
    const GFNT_Allocator * allocator, GFNT_Coverage * out_coverage,
    GFNT_Error * error) {
  static const GFNT_RasterOptions defaults = {GFNT_FILL_NONZERO, 0, 0, 0,
      GFNT_HINT_MODE_FONT, GFNT_HINT_VERSION_AUTO, NULL};
  static const GFNT_OutlineSink sink = {
    gfnt_raster_move,
    gfnt_raster_line_to,
    gfnt_raster_quad_to,
    gfnt_raster_cubic_to,
    gfnt_raster_close,
  };
  GFNT_Raster raster;
  GFNT_Coverage coverage;
  GSHCPU_Coverage computed;
  GSHCPU_Limits cpu_caps;
  GSHGEOM_Limits path_caps;
  GFNT_Limits caps;
  GFNT_Box box;
  size_t bytes = 0;
  GFNT_Result result;
  GSHCPU_Result cpu_result;

  gfnt_error_clear(error);
  if (!outline || !out_coverage) {
    return GFNT_ERR_INVALID;
  }
  if (!options) {
    options = &defaults;
  }
  if (options->fill >= GFNT_FILL_RULE_COUNT) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "the raster options name a fill rule that does not exist");
  }
  if (options->tolerance < 0
      || (options->tolerance != 0
          && options->tolerance < GFNT_RASTER_MIN_TOLERANCE)) {
    // The flattener's bound is the tolerance only from GFNT_RASTER_MIN_TOLERANCE
    // up (flatten.c), and a tolerance asked for is a promise, so a smaller one
    // is refused rather than rounded up to something else.
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "the raster options name a tolerance below the smallest the flattener "
        "can honour (GFNT_RASTER_MIN_TOLERANCE)");
  }
  if (gfnt_outline_space(outline) != GFNT_OUTLINE_PIXELS) {
    // An outline in font units would render a thousand pixels tall. Refused
    // rather than scaled here, because scaling happens once and in one place.
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "this outline is in font units; scale it to pixels before rasterising");
  }
  if (limits) {
    caps = *limits;
  }
  else {
    gfnt_limits_default(&caps);
  }
  if (!allocator) {
    allocator = gfnt_allocator_default();
  }

  memset(&coverage, 0, sizeof coverage);
  coverage.allocator = allocator;
  coverage.fill = options->fill;
  coverage.origin_x = options->origin_x;
  coverage.origin_y = options->origin_y;

  memset(&raster, 0, sizeof raster);
  raster.allocator = allocator;
  raster.error = error;
  raster.result = GFNT_OK;
  raster.tolerance = options->tolerance ? options->tolerance
                                        : GFNT_RASTER_TOLERANCE;
  raster.origin_x = options->origin_x;
  raster.origin_y = options->origin_y;

  result = gfnt_outline_bounds(outline, &box);
  if (result != GFNT_OK) {
    return result;
  }
  if (gfnt_box_is_empty(&box)) {
    // An empty glyph. A zero-sized coverage is a thing to composite.
    *out_coverage = coverage;
    return GFNT_OK;
  }
  {
    // The box, moved by the sub-pixel origin and grown to whole pixels, then
    // one pixel on every side. The margin is what makes a flattened curve
    // straying by up to the tolerance still land inside the bitmap.
    const int64_t x_min = (int64_t)box.x_min + options->origin_x;
    const int64_t y_min = (int64_t)box.y_min + options->origin_y;
    const int64_t x_max = (int64_t)box.x_max + options->origin_x;
    const int64_t y_max = (int64_t)box.y_max + options->origin_y;
    const int64_t left = gfnt_floor_div(x_min, GFNT_ONE_PIXEL) - 1;
    const int64_t bottom = gfnt_floor_div(y_min, GFNT_ONE_PIXEL) - 1;
    const int64_t right = gfnt_ceil_div(x_max, GFNT_ONE_PIXEL) + 1;
    const int64_t top = gfnt_ceil_div(y_max, GFNT_ONE_PIXEL) + 1;

    if (right - left > (int64_t)caps.max_ppem
        || top - bottom > (int64_t)caps.max_ppem) {
      return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
          "this glyph would rasterise wider or taller than max_ppem allows");
    }
    raster.left = (int32_t)left;
    raster.top = (int32_t)top;
    raster.width = (uint32_t)(right - left);
    raster.height = (uint32_t)(top - bottom);
  }

  // The same bytes the caller's limits allow, checked here against the glyph's
  // own box and with this library's messages, as it always was. shape-cpu checks
  // the rectangle again, against the same numbers (so it cannot refuse what was
  // just allowed); its cell and point caps are lifted, because the cells of one
  // glyph are bounded by the bitmap and the points by the outline, which the
  // outline's own limits (max_outline_points) already cap.
  if (!gcu_safe_mul_size((size_t)raster.width, (size_t)raster.height, &bytes)) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
        "this glyph's coverage size overflowed");
  }
  if (bytes > caps.max_raster_bytes) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
        "this glyph's coverage is larger than max_raster_bytes allows");
  }
  gshgeom_limits_default(&path_caps);
  path_caps.max_elements = SIZE_MAX;
  path_caps.max_points = SIZE_MAX;
  gshcpu_limits_default(&cpu_caps);
  cpu_caps.max_dimension = caps.max_ppem;
  cpu_caps.max_bytes = caps.max_raster_bytes;
  cpu_caps.max_cells = SIZE_MAX;
  cpu_caps.max_points = SIZE_MAX;

  if (gshgeom_device_path_create(allocator, &path_caps, &raster.path)
      != GSHGEOM_OK) {
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for a glyph's device path");
  }
  result = gfnt_outline_decompose(outline, &sink, &raster, error);
  if (result == GFNT_OK) {
    result = raster.result;
  }
  if (result == GFNT_OK) {
    // The rectangle is the bitmap, in the bitmap's own frame: its corner is
    // 0,0 and the path was placed in it above.
    cpu_result = gshcpu_coverage_fill(raster.path,
        options->fill, 0, 0, raster.width, raster.height,
        &cpu_caps, allocator, &computed);
    switch (cpu_result) {
      case GSHCPU_OK:
        gshcpu_coverage_trim(&computed);
        gfnt_raster_view(&raster, &computed, &coverage);
        break;
      case GSHCPU_ERR_OOM:
        result = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
            "no memory for a glyph's coverage");
        break;
      case GSHCPU_ERR_LIMIT:
        result = gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
            "this glyph's coverage is larger than a limit allows");
        break;
      default:
        // The bounds and the path disagreed, which is this library's bug and
        // not the font's. Reported rather than rendered, because a dropped
        // cell is a missing span and a missing span looks like a font with a
        // gap in it.
        result = gfnt_error_set(error, GFNT_ERR_INTERNAL, 0, 0,
            GFNT_GLYPH_NONE,
            "a crossing fell outside the bitmap the bounds asked for");
        break;
    }
  }
  gshgeom_device_path_destroy(raster.path);
  if (result != GFNT_OK) {
    return result;
  }
  *out_coverage = coverage;
  return GFNT_OK;
}

GFNT_Result gfnt_face_render_glyph(const GFNT_Face * face, uint32_t glyph,
    uint32_t ppem, const GFNT_RasterOptions * options,
    const GFNT_Allocator * allocator, GFNT_Coverage * out_coverage,
    GFNT_Error * error) {
  GFNT_Outline * outline = NULL;
  uint16_t upem = 0;
  GFNT_HintMode mode;
  GFNT_Result result;

  gfnt_error_clear(error);
  if (!face || !out_coverage) {
    return GFNT_ERR_INVALID;
  }
  if (ppem == 0) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, glyph,
        "a pixel size of zero cannot be rendered");
  }
  if ((size_t)ppem > face->limits.max_ppem) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, glyph,
        "this pixel size is larger than max_ppem allows");
  }
  result = gfnt_face_units_per_em(face, &upem, error);
  if (result != GFNT_OK) {
    return result;
  }
  mode = options ? options->hint_mode : GFNT_HINT_MODE_FONT;
  if (mode == GFNT_HINT_MODE_AUTO) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, 0, 0, glyph,
        "automatic hinting is reserved and not built");
  }
  if (mode != GFNT_HINT_MODE_FONT && mode != GFNT_HINT_MODE_NONE
      && mode != GFNT_HINT_MODE_FONT_STRICT) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, glyph, "not a hinting mode");
  }
  if (mode != GFNT_HINT_MODE_NONE && gfnt_face_gasp_gridfit(face, ppem)) {
    GFNT_HintContext * context = NULL;

    result = gfnt_hint_context_create(face, ppem,
        options ? options->hint_version : GFNT_HINT_VERSION_AUTO, mode,
        options ? options->variation : NULL, &context, error);
    if (result == GFNT_OK) {
      GFNT_HintStatus status = GFNT_HINT_STATUS_HINTED;

      result = gfnt_face_hinted_outline(face, context, glyph, allocator, &outline,
          &status, NULL, error);
      if (result == GFNT_OK && status == GFNT_HINT_STATUS_UNHINTED_PROGRAM_FAILED
          && mode == GFNT_HINT_MODE_FONT_STRICT) {
        gfnt_outline_destroy(outline);
        outline = NULL;
        result = gfnt_error_set(error, GFNT_ERR_CORRUPT, 0, 0, glyph,
            "a hinting program failed and the mode does not allow falling back");
      }
      gfnt_hint_context_destroy(context);
      if (result == GFNT_OK) {
        result = gfnt_raster_outline(outline, options, &face->limits, allocator,
            out_coverage, error);
      }
      gfnt_outline_destroy(outline);
      return result;
    }
    if (result != GFNT_ERR_UNSUPPORTED) {
      return result;   /* a bad glyph or a limit is the caller's to hear about */
    }
    gfnt_error_clear(error);   /* a face with no TrueType outlines has nothing to hint */
  }
  result = gfnt_face_glyph_outline(face, glyph, options ? options->variation : NULL,
      allocator, &outline, error);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_outline_scale(outline, gfnt_scale_for_ppem(upem, ppem), error);
  if (result == GFNT_OK) {
    // The face's caps, not the defaults: a face loaded under a tight
    // max_raster_bytes must not have it ignored at the point it matters.
    result = gfnt_raster_outline(outline, options, &face->limits, allocator,
        out_coverage, error);
  }
  gfnt_outline_destroy(outline);
  return result;
}

GFNT_Result gfnt_coverage_from_bitmap(const GFNT_BitmapGlyph * glyph,
    const GFNT_Allocator * allocator, GFNT_Coverage * out_coverage,
    GFNT_Error * error) {
  GFNT_Coverage coverage = {0};
  GFNT_Limits caps;
  size_t bytes;

  gfnt_error_clear(error);
  if (!glyph || !out_coverage) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no glyph, or nowhere to put the coverage");
  }
  if (glyph->bit_depth != 1) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, 0, 0, GFNT_GLYPH_NONE,
        "a strike deeper than one bit, which no container here produces yet");
  }
  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  gfnt_limits_default(&caps);

  coverage.width = glyph->width;
  coverage.height = glyph->height;
  coverage.stride = glyph->width;
  // The bearings *are* the coverage's origin, which is the whole reason this
  // conversion is three lines rather than a decision: a strike's box is already
  // positioned against the baseline in pixels, and GFNT_Coverage's left and top
  // mean the same thing.
  coverage.left = glyph->bearing_x;
  coverage.top = glyph->bearing_y;
  coverage.fill = GFNT_FILL_NONZERO;
  coverage.allocator = allocator;
  if (!gcu_safe_mul_size(coverage.stride, coverage.height, &bytes)) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
        "this glyph's coverage size overflowed");
  }
  if (bytes > caps.max_raster_bytes) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
        "this glyph's coverage is larger than max_raster_bytes allows");
  }
  coverage.data = allocator->calloc_fn(allocator->ctx, bytes ? bytes : 1u, 1u);
  if (!coverage.data) {
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for a glyph's coverage");
  }
  for (uint32_t y = 0; y < glyph->height; ++y) {
    for (uint32_t x = 0; x < glyph->width; ++x) {
      coverage.data[(size_t)y * coverage.stride + x]
          = gfnt_bitmap_pixel(glyph, x, y);
    }
  }
  *out_coverage = coverage;
  return GFNT_OK;
}
