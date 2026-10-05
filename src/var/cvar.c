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
 * The `cvt ` and `cvar` readers. See `cvt.h` in the public headers.
 *
 * A `cvar` is a tuple variation store like `gvar`'s with one difference that
 * matters: a tuple's deltas apply to **control values**, one delta per value
 * rather than an x and a y per point, and a value a tuple does not name is not
 * interpolated from its neighbours - control values have no contours. The
 * parsing of point numbers and packed deltas is `gvar`'s, shared, because two
 * copies of a bit-level format are two places to be wrong in different ways.
 */

#include <ghoti.io/font/cvt.h>
#include <ghoti.io/font/macros.h>
#include <string.h>
#include "../core/fixed.h"
#include "../reader/reader.h"
#include "../sfnt/sfnt.h"
#include "../tables/tables.h"
#include "gvar.h"
#include "var.h"

#define GFNT_TAG_CVT GFNT_TAG('c', 'v', 't', ' ')
#define GFNT_TAG_CVAR GFNT_TAG('c', 'v', 'a', 'r')

GFNT_Result gfnt_face_cvt_count(const GFNT_Face * face, size_t * out_count,
    GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_Result result;

  gfnt_error_clear(error);
  if (!face || !out_count) {
    return GFNT_ERR_INVALID;
  }
  if (!gfnt_face_has_table(face, GFNT_TAG_CVT)) {
    *out_count = 0;
    return GFNT_OK;
  }
  result = gfnt_face_table_reader(face, GFNT_TAG_CVT, &table, error);
  if (result != GFNT_OK) {
    return result;
  }
  // Two bytes each; a trailing odd byte is not a value.
  *out_count = table.length / 2u;
  return GFNT_OK;
}

/**
 * Add every applicable tuple's deltas into @p acc, one per control value.
 *
 * @param acc @p count entries, zeroed by the caller; receives the sum with
 *   ::GFNT_GVAR_FRACTION_BITS fractional bits.
 */
static GFNT_Result gfnt_cvar_deltas(const GFNT_Face * face,
    const GFNT_F2Dot14 * coordinates, size_t coordinate_count, size_t count,
    int64_t * acc, GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_Reader headers;
  GFNT_Reader serial;
  const GFNT_Fvar * fvar = NULL;
  const GFNT_Allocator * allocator = face->allocator;
  uint16_t major = 0;
  uint16_t tuple_word = 0;
  uint16_t data_offset = 0;
  uint32_t * numbers = NULL;
  uint32_t * shared = NULL;
  int32_t * deltas = NULL;
  int16_t * peak = NULL;
  int16_t * low = NULL;
  int16_t * high = NULL;
  size_t shared_count = 0;
  bool shared_all = true;
  bool shared_points;
  size_t axes;
  GFNT_Result result;

  result = gfnt_face_fvar(face, &fvar, error);
  if (result == GFNT_OK) {
    result = gfnt_face_table_reader(face, GFNT_TAG_CVAR, &table, error);
  }
  if (result != GFNT_OK) {
    return result;
  }
  table.error = error;
  axes = fvar->axis_count;
  if (gfnt_reader_u16_at(&table, 0, &major) != GFNT_OK
      || gfnt_reader_u16_at(&table, 4, &tuple_word) != GFNT_OK
      || gfnt_reader_u16_at(&table, 6, &data_offset) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CVAR, 0,
        GFNT_GLYPH_NONE, "a cvar shorter than its own header");
  }
  if (major != 1) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_CVAR, 0,
        GFNT_GLYPH_NONE, "a cvar version other than 1.x");
  }
  shared_points = (tuple_word & GFNT_GVAR_SHARED_POINT_NUMBERS) != 0;
  result = gfnt_reader_sub(&table, 8, GFNT_READER_REST, &headers);
  if (result == GFNT_OK) {
    result = gfnt_reader_sub(&table, data_offset, GFNT_READER_REST, &serial);
  }
  if (result != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CVAR, 6,
        GFNT_GLYPH_NONE, "a cvar whose data begins past its own end");
  }
  headers.error = error;
  serial.error = error;

  numbers = allocator->calloc_fn(allocator->ctx, count ? count : 1u,
      sizeof *numbers);
  shared = allocator->calloc_fn(allocator->ctx, count ? count : 1u,
      sizeof *shared);
  deltas = allocator->calloc_fn(allocator->ctx, count ? count : 1u,
      sizeof *deltas);
  peak = allocator->calloc_fn(allocator->ctx, axes ? axes : 1u, sizeof *peak);
  low = allocator->calloc_fn(allocator->ctx, axes ? axes : 1u, sizeof *low);
  high = allocator->calloc_fn(allocator->ctx, axes ? axes : 1u, sizeof *high);
  if (!numbers || !shared || !deltas || !peak || !low || !high) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_CVAR, 0,
        GFNT_GLYPH_NONE, "no memory for the control value deltas");
    goto done;
  }
  if (shared_points) {
    result = gfnt_tuple_points(&serial, count, shared, &shared_count,
        &shared_all, GFNT_TAG_CVAR, GFNT_GLYPH_NONE, error);
    if (result != GFNT_OK) {
      goto done;
    }
  }
  for (size_t t = 0; t < (size_t)(tuple_word & GFNT_GVAR_COUNT_MASK); ++t) {
    uint16_t size = 0;
    uint16_t index = 0;
    bool intermediate;
    GFNT_Reader tuple;
    size_t named;
    bool all;
    const uint32_t * list;
    int64_t scalar;

    if (gfnt_read_u16(&headers, &size) != GFNT_OK
        || gfnt_read_u16(&headers, &index) != GFNT_OK) {
      result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CVAR,
          gfnt_reader_tell(&headers), GFNT_GLYPH_NONE,
          "a cvar tuple header ends past the table");
      goto done;
    }
    if (!(index & GFNT_GVAR_EMBEDDED_PEAK_TUPLE)) {
      // `cvar` has no shared tuples to point at.
      result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CVAR,
          gfnt_reader_tell(&headers), GFNT_GLYPH_NONE,
          "a cvar tuple without an embedded peak, and cvar has no shared tuples");
      goto done;
    }
    intermediate = (index & GFNT_GVAR_INTERMEDIATE_REGION) != 0;
    for (size_t a = 0; a < axes; ++a) {
      if (gfnt_read_f2dot14(&headers, &peak[a]) != GFNT_OK) {
        result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CVAR,
            gfnt_reader_tell(&headers), GFNT_GLYPH_NONE,
            "a cvar tuple header ends past the table");
        goto done;
      }
    }
    for (size_t a = 0; intermediate && a < 2u * axes; ++a) {
      if (gfnt_read_f2dot14(&headers, a < axes ? &low[a] : &high[a - axes])
          != GFNT_OK) {
        result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CVAR,
            gfnt_reader_tell(&headers), GFNT_GLYPH_NONE,
            "a cvar tuple header ends past the table");
        goto done;
      }
    }
    result = gfnt_reader_sub(&serial, gfnt_reader_tell(&serial), size, &tuple);
    if (result == GFNT_OK) {
      result = gfnt_reader_skip(&serial, size);
    }
    if (result != GFNT_OK) {
      goto done;
    }
    tuple.error = error;
    scalar = gfnt_gvar_scalar(axes, coordinates, coordinate_count, peak,
        intermediate ? low : NULL, intermediate ? high : NULL);
    if (scalar == 0) {
      continue;
    }
    if (index & GFNT_GVAR_PRIVATE_POINT_NUMBERS) {
      result = gfnt_tuple_points(&tuple, count, numbers, &named, &all,
          GFNT_TAG_CVAR, GFNT_GLYPH_NONE, error);
      if (result != GFNT_OK) {
        goto done;
      }
      list = numbers;
    }
    else {
      named = shared_points ? shared_count : count;
      all = !shared_points || shared_all;
      list = shared;
    }
    result = gfnt_tuple_deltas(&tuple, named, deltas, GFNT_TAG_CVAR,
        GFNT_GLYPH_NONE, error);
    if (result != GFNT_OK) {
      goto done;
    }
    for (size_t i = 0; i < named; ++i) {
      acc[all ? i : list[i]] += (int64_t)deltas[i] * scalar;
    }
  }
  result = GFNT_OK;

done:
  allocator->free_fn(allocator->ctx, numbers);
  allocator->free_fn(allocator->ctx, shared);
  allocator->free_fn(allocator->ctx, deltas);
  allocator->free_fn(allocator->ctx, peak);
  allocator->free_fn(allocator->ctx, low);
  allocator->free_fn(allocator->ctx, high);
  return result;
}

GFNT_Result gfnt_face_cvt_values(const GFNT_Face * face,
    const GFNT_Variation * variation, int32_t * out_values, size_t capacity,
    GFNT_Error * error) {
  GFNT_Reader table;
  size_t count = 0;
  bool moves = false;
  int64_t * acc = NULL;
  GFNT_Result result;

  if (!face || (!out_values && capacity > 0)) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_face_cvt_count(face, &count, error);
  if (result == GFNT_OK) {
    result = gfnt_variation_moves(face, GFNT_GLYPH_NONE, variation, &moves,
        error);
  }
  if (result != GFNT_OK) {
    return result;
  }
  if (capacity < count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_CVT, 0,
        GFNT_GLYPH_NONE, "an output too small for the control value count");
  }
  if (count == 0) {
    return GFNT_OK;
  }
  result = gfnt_face_table_reader(face, GFNT_TAG_CVT, &table, error);
  if (result != GFNT_OK) {
    return result;
  }
  table.error = error;
  for (size_t i = 0; i < count; ++i) {
    int16_t value = 0;

    if (gfnt_reader_s16_at(&table, 2u * i, &value) != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_CVT, 2u * i,
          GFNT_GLYPH_NONE, "a cvt shorter than its own length");
    }
    out_values[i] = value;
  }
  // The default location, and a font that states no variation: the table's own.
  if (!moves || !gfnt_face_has_table(face, GFNT_TAG_CVAR)) {
    return GFNT_OK;
  }
  acc = face->allocator->calloc_fn(face->allocator->ctx, count, sizeof *acc);
  if (!acc) {
    return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_CVAR, 0,
        GFNT_GLYPH_NONE, "no memory for the control value deltas");
  }
  result = gfnt_cvar_deltas(face, variation->coords, variation->count, count,
      acc, error);
  if (result == GFNT_OK) {
    for (size_t i = 0; i < count; ++i) {
      int64_t moved = (int64_t)out_values[i]
          + gfnt_round_shift(gfnt_clamp64(acc[i]), GFNT_GVAR_FRACTION_BITS);

      out_values[i] = (int32_t)(moved > INT32_MAX ? INT32_MAX
          : moved < INT32_MIN ? INT32_MIN : moved);
    }
  }
  face->allocator->free_fn(face->allocator->ctx, acc);
  return result;
}
