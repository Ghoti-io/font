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
 * The scan converter: cells, the sweep, and the fill rule.
 *
 * documentation/design.md section 8.1. Every edge of the flattened path
 * deposits two signed numbers into each cell it crosses:
 *
 * - `cover`, the signed height of the crossing in 26.6 units, and
 * - `area`, that height times the sum of the crossing's two x positions
 *   within the cell.
 *
 * A sweep along each row then accumulates `cover` from left to right, which is
 * the winding number times a pixel's height, and `area` corrects the cell the
 * edge actually passes through. This is FreeType's `ftgrays` arithmetic with
 * `PIXEL_BITS` of 6 rather than 8, because 26.6 is this library's pixel space
 * everywhere else and a second sub-pixel resolution would be a second rounding
 * rule.
 *
 * Nothing here is floating point, and nothing here reads the host's byte order.
 */

#include <ghoti.io/font/macros.h>
#include <ghoti.io/cutil/safemath.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/raster.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "../core/fixed.h"
#include "../outline/outline.h"
#include "../sfnt/sfnt.h"

/** Sub-pixel bits: 26.6, the same everywhere in this library. */
#define GFNT_PIXEL_BITS 6
/** One pixel in 26.6. */
#define GFNT_ONE_PIXEL 64

/**
 * The shift that turns an accumulated area into 0..256.
 *
 * A fully covered pixel accumulates `2 * ONE_PIXEL * ONE_PIXEL`, and 256 is
 * full coverage, so the shift is `2 * PIXEL_BITS + 1 - 8`. Spelled from
 * ::GFNT_PIXEL_BITS rather than as 5, so that changing the sub-pixel
 * resolution cannot leave this behind.
 */
#define GFNT_AREA_SHIFT (2 * GFNT_PIXEL_BITS + 1 - 8)

/** Default flattening tolerance: a sixteenth of a pixel. */
#define GFNT_RASTER_TOLERANCE (GFNT_ONE_PIXEL / 16)

/** How deep a curve may be subdivided before its chord is used as it is. */
#define GFNT_RASTER_DEPTH 16

/** Smallest cell allocation. */
#define GFNT_RASTER_MIN_CELLS 64u

/**
 * One pixel's accumulated crossings.
 *
 * `cover` and `area` are 64-bit because they are sums over every edge crossing
 * one pixel: an outline with `max_outline_points` points could cross one cell
 * that many times, and a 32-bit sum of `65536 * 64 * 128` would be within a
 * factor of eight of overflowing rather than comfortably clear of it.
 */
typedef struct GFNT_Cell {
  int32_t x;     ///< Column, from the raster's left edge.
  int32_t y;     ///< Row, from the raster's top.
  int64_t cover; ///< Signed height of the crossings, 26.6.
  int64_t area;  ///< Sum of height times the two x positions in the cell.
} GFNT_Cell;

/**
 * The rasteriser's working state.
 */
typedef struct GFNT_Raster {
  const GFNT_Allocator * allocator;
  GFNT_Error * error;
  GFNT_Result result;        ///< The first failure, if any.

  GFNT_Cell * cells;
  size_t cell_count;
  size_t cell_capacity;

  int32_t left;              ///< Pixel column the bitmap starts at.
  int32_t top;               ///< Pixel row the bitmap starts at, y-down.
  uint32_t width;
  uint32_t height;

  GFNT_F26Dot6 tolerance;
  GFNT_F26Dot6 origin_x;     ///< The sub-pixel offset the glyph is drawn at.
  GFNT_F26Dot6 origin_y;
  GFNT_Point current;        ///< Where the pen is, 26.6, bitmap-relative.
  GFNT_Point contour_start;  ///< Where this contour began.
  bool dropped;              ///< A cell fell outside the bitmap.
} GFNT_Raster;

const char * gfnt_fill_rule_string(GFNT_FillRule rule) {
  switch (rule) {
    case GFNT_FILL_NONZERO:
      return "non-zero";
    case GFNT_FILL_EVEN_ODD:
      return "even-odd";
    case GFNT_FILL_RULE_COUNT:
      break;
  }
  return "unknown fill rule";
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

// --- cells -----------------------------------------------------------------

/**
 * Add a crossing to the cell at (@p ex, @p ey).
 *
 * @param dy The signed height crossed, 26.6.
 * @param fx1 Where the crossing entered, within the cell, 0..ONE_PIXEL.
 * @param fx2 Where it left.
 */
static void gfnt_raster_cell(GFNT_Raster * raster, int32_t ex, int32_t ey,
    int32_t dy, int32_t fx1, int32_t fx2) {
  GFNT_Cell * cell = NULL;

  if (dy == 0) {
    // A horizontal crossing contributes no winding and no area. Skipped rather
    // than recorded, because a cell of zeroes changes the sweep's cell list
    // without changing its answer.
    return;
  }
  if (ex < 0 || ey < 0 || (uint32_t)ex >= raster->width
      || (uint32_t)ey >= raster->height) {
    // Cannot happen with correct bounds: the bitmap is the outline's own box
    // grown by a pixel on every side, and the flattener strays by at most the
    // tolerance. Recorded rather than ignored so that a bounds bug is a
    // reported failure and not a missing span.
    raster->dropped = true;
    return;
  }
  if (raster->cell_count > 0) {
    GFNT_Cell * last = &raster->cells[raster->cell_count - 1];

    if (last->x == ex && last->y == ey) {
      // The common case by far: a walk along one edge stays in a cell for
      // several steps. Merging here keeps the array small enough that the
      // sort afterwards is cheap.
      last->cover += dy;
      last->area += (int64_t)dy * (fx1 + fx2);
      return;
    }
  }
  if (raster->cell_count == raster->cell_capacity) {
    size_t capacity = raster->cell_capacity ? raster->cell_capacity * 2
                                            : GFNT_RASTER_MIN_CELLS;
    size_t bytes = 0;
    void * grown = NULL;

    if (!gcu_safe_mul_size(capacity, sizeof *raster->cells, &bytes)) {
      raster->result = gfnt_error_set(raster->error, GFNT_ERR_OOM, 0, 0,
          GFNT_GLYPH_NONE, "the rasteriser's cell array overflowed");
      return;
    }
    grown = raster->allocator->realloc_fn(raster->allocator->ctx,
        raster->cells, bytes);
    if (!grown) {
      raster->result = gfnt_error_set(raster->error, GFNT_ERR_OOM, 0, 0,
          GFNT_GLYPH_NONE, "no memory for the rasteriser's cells");
      return;
    }
    raster->cells = grown;
    raster->cell_capacity = capacity;
  }
  cell = &raster->cells[raster->cell_count++];
  cell->x = ex;
  cell->y = ey;
  cell->cover = dy;
  cell->area = (int64_t)dy * (fx1 + fx2);
}

/**
 * Accumulate one piece of an edge that lies within a single row.
 *
 * Both endpoints are bitmap-relative 26.6 and share a row.
 */
static void gfnt_raster_scanline(GFNT_Raster * raster, int32_t ey,
    GFNT_Point from, GFNT_Point to) {
  int32_t ex1 = (int32_t)gfnt_floor_div(from.x, GFNT_ONE_PIXEL);
  int32_t ex2 = (int32_t)gfnt_floor_div(to.x, GFNT_ONE_PIXEL);
  int32_t dx = to.x - from.x;
  int32_t dy = to.y - from.y;
  int32_t step;
  int32_t ex;
  GFNT_Point cursor = from;

  if (ex1 == ex2) {
    gfnt_raster_cell(raster, ex1, ey, dy,
        from.x - ex1 * GFNT_ONE_PIXEL, to.x - ex1 * GFNT_ONE_PIXEL);
    return;
  }
  // The edge crosses at least one column boundary. Each boundary's y is found
  // once and shared by the pieces either side of it, so the walk stays closed:
  // rounding a boundary twice is how a seam appears between two columns.
  step = dx > 0 ? 1 : -1;
  for (ex = ex1; ex != ex2; ex += step) {
    const int32_t boundary = (step > 0 ? ex + 1 : ex) * GFNT_ONE_PIXEL;
    const int32_t at_y = (int32_t)(from.y
        + gfnt_round_div((int64_t)(boundary - from.x) * dy, dx));
    GFNT_Point split;

    split.x = boundary;
    split.y = at_y;
    gfnt_raster_cell(raster, ex, ey, split.y - cursor.y,
        cursor.x - ex * GFNT_ONE_PIXEL, split.x - ex * GFNT_ONE_PIXEL);
    cursor = split;
  }
  gfnt_raster_cell(raster, ex2, ey, to.y - cursor.y,
      cursor.x - ex2 * GFNT_ONE_PIXEL, to.x - ex2 * GFNT_ONE_PIXEL);
}

/**
 * Accumulate one edge of the flattened path.
 *
 * Rows are y-down here: the outline was flipped on the way in, so that the
 * sweep writes row 0 first and the flip lives in one place.
 */
static void gfnt_raster_line(GFNT_Raster * raster, GFNT_Point to) {
  GFNT_Point from = raster->current;
  int32_t ey1;
  int32_t ey2;
  int32_t dy;
  int32_t dx;
  int32_t step;
  int32_t ey;
  GFNT_Point cursor;

  raster->current = to;
  if (from.y == to.y) {
    // A horizontal edge crosses no scanline and so contributes nothing. This
    // is not an optimisation: giving it a row to belong to would require
    // choosing one, and either choice puts area in a cell the edge does not
    // bound.
    return;
  }
  ey1 = (int32_t)gfnt_floor_div(from.y, GFNT_ONE_PIXEL);
  ey2 = (int32_t)gfnt_floor_div(to.y, GFNT_ONE_PIXEL);
  if (ey1 == ey2) {
    gfnt_raster_scanline(raster, ey1, from, to);
    return;
  }
  dy = to.y - from.y;
  dx = to.x - from.x;
  step = dy > 0 ? 1 : -1;
  cursor = from;
  for (ey = ey1; ey != ey2; ey += step) {
    const int32_t boundary = (step > 0 ? ey + 1 : ey) * GFNT_ONE_PIXEL;
    const int32_t at_x = (int32_t)(from.x
        + gfnt_round_div((int64_t)(boundary - from.y) * dx, dy));
    GFNT_Point split;

    split.x = at_x;
    split.y = boundary;
    gfnt_raster_scanline(raster, ey, cursor, split);
    cursor = split;
  }
  gfnt_raster_scanline(raster, ey2, cursor, to);
}

// --- flattening ------------------------------------------------------------

/** The midpoint of two points, floored, as the outline walk uses. */
static GFNT_Point gfnt_raster_mid(GFNT_Point a, GFNT_Point b) {
  GFNT_Point mid;

  mid.x = (GFNT_F26Dot6)gfnt_floor_div((int64_t)a.x + b.x, 2);
  mid.y = (GFNT_F26Dot6)gfnt_floor_div((int64_t)a.y + b.y, 2);
  return mid;
}

static int32_t gfnt_abs32(int32_t value) {
  return value < 0 ? -value : value;
}

/**
 * Flatten a quadratic, splitting at its midpoint until it is flat enough.
 *
 * The deviation of a quadratic from its chord is
 * `|2*p1 - p0 - p2| / 4` at its midpoint, so the test is that sum against four
 * tolerances. Every split is an exact halving of integers, and the depth cap
 * is what a hostile curve runs into rather than the stack.
 */
static void gfnt_raster_quad(GFNT_Raster * raster, GFNT_Point p0,
    GFNT_Point p1, GFNT_Point p2, int depth) {
  const int32_t dx = gfnt_abs32(2 * p1.x - p0.x - p2.x);
  const int32_t dy = gfnt_abs32(2 * p1.y - p0.y - p2.y);

  if (depth <= 0 || dx + dy <= 4 * raster->tolerance) {
    gfnt_raster_line(raster, p2);
    return;
  }
  {
    const GFNT_Point a = gfnt_raster_mid(p0, p1);
    const GFNT_Point b = gfnt_raster_mid(p1, p2);
    const GFNT_Point m = gfnt_raster_mid(a, b);

    gfnt_raster_quad(raster, p0, a, m, depth - 1);
    gfnt_raster_quad(raster, m, b, p2, depth - 1);
  }
}

/**
 * Flatten a cubic the same way.
 *
 * A cubic's deviation from its chord is bounded by three quarters of the larger
 * of its two control-point deviations, so both are measured.
 */
static void gfnt_raster_cubic(GFNT_Raster * raster, GFNT_Point p0,
    GFNT_Point p1, GFNT_Point p2, GFNT_Point p3, int depth) {
  const int32_t d1 = gfnt_abs32(3 * p1.x - 2 * p0.x - p3.x)
      + gfnt_abs32(3 * p1.y - 2 * p0.y - p3.y);
  const int32_t d2 = gfnt_abs32(3 * p2.x - p0.x - 2 * p3.x)
      + gfnt_abs32(3 * p2.y - p0.y - 2 * p3.y);

  if (depth <= 0 || (d1 <= 4 * raster->tolerance
          && d2 <= 4 * raster->tolerance)) {
    gfnt_raster_line(raster, p3);
    return;
  }
  {
    const GFNT_Point a = gfnt_raster_mid(p0, p1);
    const GFNT_Point b = gfnt_raster_mid(p1, p2);
    const GFNT_Point c = gfnt_raster_mid(p2, p3);
    const GFNT_Point d = gfnt_raster_mid(a, b);
    const GFNT_Point e = gfnt_raster_mid(b, c);
    const GFNT_Point m = gfnt_raster_mid(d, e);

    gfnt_raster_cubic(raster, p0, a, d, m, depth - 1);
    gfnt_raster_cubic(raster, m, e, c, p3, depth - 1);
  }
}

// --- the sink the outline walk feeds ---------------------------------------

/**
 * Turn a glyph-space point into a bitmap-space one.
 *
 * The one place y is flipped: the outline is y-up (design.md section 5.5) and
 * row 0 of the coverage is its top. Written as a subtraction from the top edge
 * rather than as a negation plus an offset, because the two differ by a pixel
 * and only one of them puts the top row at row zero.
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

static GFNT_Result gfnt_raster_move(void * user, GFNT_Point to) {
  GFNT_Raster * raster = user;
  const GFNT_Point placed = gfnt_raster_place(raster, to);

  // A contour that has not been closed by the walk cannot happen -
  // gfnt_outline_decompose() closes every one - but the close is what makes
  // the winding number well defined, so the start is remembered here and used
  // there rather than assumed.
  raster->contour_start = placed;
  raster->current = placed;
  return raster->result;
}

static GFNT_Result gfnt_raster_line_to(void * user, GFNT_Point to) {
  GFNT_Raster * raster = user;

  gfnt_raster_line(raster, gfnt_raster_place(raster, to));
  return raster->result;
}

static GFNT_Result gfnt_raster_quad_to(void * user, GFNT_Point control,
    GFNT_Point to) {
  GFNT_Raster * raster = user;

  gfnt_raster_quad(raster, raster->current,
      gfnt_raster_place(raster, control), gfnt_raster_place(raster, to),
      GFNT_RASTER_DEPTH);
  return raster->result;
}

static GFNT_Result gfnt_raster_cubic_to(void * user, GFNT_Point c1,
    GFNT_Point c2, GFNT_Point to) {
  GFNT_Raster * raster = user;

  gfnt_raster_cubic(raster, raster->current, gfnt_raster_place(raster, c1),
      gfnt_raster_place(raster, c2), gfnt_raster_place(raster, to),
      GFNT_RASTER_DEPTH);
  return raster->result;
}

static GFNT_Result gfnt_raster_close(void * user) {
  GFNT_Raster * raster = user;

  gfnt_raster_line(raster, raster->contour_start);
  return raster->result;
}

// --- the sweep -------------------------------------------------------------

/**
 * Sort the cells by row and then by column, merging the duplicates.
 *
 * A counting sort by row and an insertion sort within each row, both written
 * here rather than handed to `qsort`: the comparison order of equal keys is
 * unspecified for `qsort`, and while adding integers is commutative - so the
 * merged totals would be the same either way - the determinism claim is easier
 * to hold when the sort is not a platform's.
 */
static GFNT_Result gfnt_raster_sort(GFNT_Raster * raster, size_t ** out_rows) {
  size_t * rows = NULL;
  GFNT_Cell * sorted = NULL;
  size_t bytes = 0;

  // rows[y] is where row y starts, and rows[height] closes the last one.
  if (!gcu_safe_mul_size((size_t)raster->height + 2u, sizeof *rows, &bytes)) {
    return gfnt_error_set(raster->error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "the rasteriser's row index overflowed");
  }
  rows = raster->allocator->calloc_fn(raster->allocator->ctx,
      (size_t)raster->height + 2u, sizeof *rows);
  if (!rows) {
    return gfnt_error_set(raster->error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for the rasteriser's row index");
  }
  if (raster->cell_count == 0) {
    *out_rows = rows;
    return GFNT_OK;
  }
  if (!gcu_safe_mul_size(raster->cell_count, sizeof *sorted, &bytes)) {
    raster->allocator->free_fn(raster->allocator->ctx, rows);
    return gfnt_error_set(raster->error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "the rasteriser's cell array overflowed");
  }
  sorted = raster->allocator->malloc_fn(raster->allocator->ctx, bytes);
  if (!sorted) {
    raster->allocator->free_fn(raster->allocator->ctx, rows);
    return gfnt_error_set(raster->error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory to sort the rasteriser's cells");
  }

  for (size_t i = 0; i < raster->cell_count; ++i) {
    rows[(size_t)raster->cells[i].y + 1u] += 1;
  }
  for (uint32_t y = 0; y < raster->height + 1u; ++y) {
    rows[y + 1u] += rows[y];
  }
  {
    // A second copy of the starts, consumed while filling; rows[] itself has
    // to survive as the index the sweep reads.
    size_t * cursor = raster->allocator->calloc_fn(raster->allocator->ctx,
        (size_t)raster->height + 2u, sizeof *cursor);

    if (!cursor) {
      raster->allocator->free_fn(raster->allocator->ctx, sorted);
      raster->allocator->free_fn(raster->allocator->ctx, rows);
      return gfnt_error_set(raster->error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
          "no memory to sort the rasteriser's cells");
    }
    memcpy(cursor, rows, ((size_t)raster->height + 2u) * sizeof *cursor);
    for (size_t i = 0; i < raster->cell_count; ++i) {
      sorted[cursor[(size_t)raster->cells[i].y]++] = raster->cells[i];
    }
    raster->allocator->free_fn(raster->allocator->ctx, cursor);
  }
  memcpy(raster->cells, sorted, raster->cell_count * sizeof *sorted);
  raster->allocator->free_fn(raster->allocator->ctx, sorted);

  // Within each row: insertion sort by column, merging equal columns as they
  // meet. Rows are short - a cell per column crossed - so this is linear in
  // practice and needs no scratch.
  for (uint32_t y = 0; y < raster->height; ++y) {
    size_t first = rows[y];
    size_t last = rows[y + 1u];

    for (size_t i = first + 1; i < last; ++i) {
      GFNT_Cell held = raster->cells[i];
      size_t j = i;

      while (j > first && raster->cells[j - 1].x > held.x) {
        raster->cells[j] = raster->cells[j - 1];
        j -= 1;
      }
      raster->cells[j] = held;
    }
    // Merge equal columns, compacting the row in place. The slots the merge
    // frees stay in the row's span, so they are filled with a sentinel column
    // that sorts after every real one and carries no cover: the sweep stops at
    // the first of them. Shrinking the span instead would mean a second array
    // of per-row counts for no gain.
    {
      size_t write = first;

      for (size_t read = first; read < last; ++read) {
        if (write > first
            && raster->cells[write - 1].x == raster->cells[read].x) {
          raster->cells[write - 1].cover += raster->cells[read].cover;
          raster->cells[write - 1].area += raster->cells[read].area;
          continue;
        }
        raster->cells[write++] = raster->cells[read];
      }
      for (size_t fill = write; fill < last; ++fill) {
        raster->cells[fill].x = INT32_MAX;
        raster->cells[fill].cover = 0;
        raster->cells[fill].area = 0;
      }
    }
  }
  *out_rows = rows;
  return GFNT_OK;
}

/**
 * Turn an accumulated area into 8-bit coverage under the fill rule.
 */
static uint8_t gfnt_raster_coverage(int64_t area, GFNT_FillRule fill) {
  int64_t value = area >> GFNT_AREA_SHIFT;

  if (value < 0) {
    value = -value;
  }
  if (fill == GFNT_FILL_EVEN_ODD) {
    // Winding 2 is outside again, so the ramp folds every 512.
    value &= 511;
    if (value > 256) {
      value = 512 - value;
    }
    else if (value == 256) {
      value = 255;
    }
  }
  else if (value >= 256) {
    value = 255;
  }
  return (uint8_t)value;
}

/**
 * Write a run of pixels at one coverage.
 */
static void gfnt_raster_span(GFNT_Coverage * coverage, uint32_t row,
    int32_t from, int32_t count, int64_t area) {
  const uint8_t value = gfnt_raster_coverage(area, coverage->fill);
  uint8_t * line;

  if (value == 0 || count <= 0 || from >= (int32_t)coverage->width) {
    return;
  }
  if (from < 0) {
    count += from;
    from = 0;
  }
  if (from + count > (int32_t)coverage->width) {
    count = (int32_t)coverage->width - from;
  }
  if (count <= 0) {
    return;
  }
  line = coverage->data + (size_t)row * coverage->stride;
  memset(line + from, value, (size_t)count);
}

/**
 * Walk each row's cells, accumulating winding and writing spans.
 */
static void gfnt_raster_sweep(GFNT_Raster * raster, const size_t * rows,
    GFNT_Coverage * coverage) {
  for (uint32_t y = 0; y < raster->height; ++y) {
    int64_t cover = 0;
    int32_t x = 0;

    for (size_t index = rows[y]; index < rows[y + 1u]; ++index) {
      const GFNT_Cell * cell = &raster->cells[index];

      if (cell->x == INT32_MAX) {
        break;
      }
      if (cover != 0 && cell->x > x) {
        gfnt_raster_span(coverage, y, x, cell->x - x, cover);
      }
      cover += cell->cover * (GFNT_ONE_PIXEL * 2);
      gfnt_raster_span(coverage, y, cell->x, 1, cover - cell->area);
      x = cell->x + 1;
    }
    // No fill after the last cell. FreeType's sweep has one, because it clips
    // cells to the bitmap and so loses the closing edge of a shape that reaches
    // the right margin; here the bitmap is the outline's own box grown by a
    // pixel on every side, so every closing edge has a cell of its own inside
    // it and the accumulated cover is back to zero by the end of every row. A
    // planted defect proved the branch unreachable rather than untested: it was
    // removed instead of being left as code nothing can run.
  }
}

/**
 * Shrink the coverage to the pixels that actually have any.
 *
 * The bitmap is sized from the outline's box grown by a pixel on every side,
 * which is what makes a flattened curve straying by up to the tolerance land
 * inside it. That margin is scaffolding, not output: an atlas allocator that
 * was handed a two-pixel border on every glyph would pay for it in every
 * texture. So the rows are compacted in place - one buffer, one pass - and
 * `left` and `top` move to match, which is what makes them mean anything.
 *
 * A coverage with no non-zero pixel becomes 0x0 with its pixels released,
 * which is the same answer an empty glyph gives: there is nothing to composite,
 * and a caller should not have to tell the two apart.
 */
static void gfnt_raster_trim(GFNT_Coverage * coverage) {
  uint32_t first_row = coverage->height;
  uint32_t last_row = 0;
  uint32_t first_column = coverage->width;
  uint32_t last_column = 0;
  uint32_t width;
  uint32_t height;

  for (uint32_t row = 0; row < coverage->height; ++row) {
    const uint8_t * line = coverage->data + (size_t)row * coverage->stride;

    for (uint32_t column = 0; column < coverage->width; ++column) {
      if (line[column] == 0) {
        continue;
      }
      if (row < first_row) {
        first_row = row;
      }
      last_row = row;
      if (column < first_column) {
        first_column = column;
      }
      if (column > last_column) {
        last_column = column;
      }
    }
  }
  if (first_row > last_row || first_column > last_column) {
    const GFNT_Allocator * allocator = coverage->allocator;

    allocator->free_fn(allocator->ctx, coverage->data);
    coverage->data = NULL;
    coverage->width = 0;
    coverage->height = 0;
    coverage->stride = 0;
    coverage->left = 0;
    coverage->top = 0;
    return;
  }
  width = last_column - first_column + 1u;
  height = last_row - first_row + 1u;
  for (uint32_t row = 0; row < height; ++row) {
    memmove(coverage->data + (size_t)row * width,
        coverage->data + (size_t)(row + first_row) * coverage->stride
            + first_column,
        width);
  }
  coverage->left += (int32_t)first_column;
  coverage->top -= (int32_t)first_row;
  coverage->width = width;
  coverage->height = height;
  coverage->stride = width;
}

// --- the entry points ------------------------------------------------------

GFNT_Result gfnt_raster_outline(const GFNT_Outline * outline,
    const GFNT_RasterOptions * options, const GFNT_Limits * limits,
    const GFNT_Allocator * allocator, GFNT_Coverage * out_coverage,
    GFNT_Error * error) {
  static const GFNT_RasterOptions defaults = {GFNT_FILL_NONZERO, 0, 0, 0};
  static const GFNT_OutlineSink sink = {
    gfnt_raster_move,
    gfnt_raster_line_to,
    gfnt_raster_quad_to,
    gfnt_raster_cubic_to,
    gfnt_raster_close,
  };
  GFNT_Raster raster;
  GFNT_Coverage coverage;
  GFNT_Limits caps;
  GFNT_Box box;
  size_t * rows = NULL;
  size_t bytes = 0;
  GFNT_Result result;

  gfnt_error_clear(error);
  if (!outline || !out_coverage) {
    return GFNT_ERR_INVALID;
  }
  if (!options) {
    options = &defaults;
  }
  if (options->fill >= GFNT_FILL_RULE_COUNT || options->tolerance < 0) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "the raster options name a fill rule or tolerance that does not exist");
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

  coverage.width = raster.width;
  coverage.height = raster.height;
  coverage.stride = raster.width;
  coverage.left = raster.left;
  coverage.top = raster.top;
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

  result = gfnt_outline_decompose(outline, &sink, &raster, error);
  if (result == GFNT_OK) {
    result = raster.result;
  }
  if (result == GFNT_OK && raster.dropped) {
    // The bounds and the flattener disagreed, which is this library's bug and
    // not the font's. Reported rather than rendered, because a dropped cell is
    // a missing span and a missing span looks like a font with a gap in it.
    result = gfnt_error_set(error, GFNT_ERR_INTERNAL, 0, 0, GFNT_GLYPH_NONE,
        "a crossing fell outside the bitmap the bounds asked for");
  }
  if (result == GFNT_OK) {
    result = gfnt_raster_sort(&raster, &rows);
  }
  if (result == GFNT_OK) {
    gfnt_raster_sweep(&raster, rows, &coverage);
    gfnt_raster_trim(&coverage);
  }
  allocator->free_fn(allocator->ctx, rows);
  allocator->free_fn(allocator->ctx, raster.cells);
  if (result != GFNT_OK) {
    gfnt_coverage_destroy(&coverage);
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
  result = gfnt_face_glyph_outline(face, glyph, NULL, allocator, &outline,
      error);
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
