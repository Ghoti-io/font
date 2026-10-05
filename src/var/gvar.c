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
 * The `gvar` reader: tuple headers, packed point numbers, packed deltas, tuple
 * scalars, and the interpolation of untouched points.
 *
 * documentation/design.md section 7.7, and `gvar.h` for what is and is not this
 * file's business.
 *
 * **The arithmetic is exact until it is not possible to be.** A delta is a whole
 * number of font units and a tuple's scalar is a fraction carried to
 * ::GFNT_GVAR_FRACTION_BITS bits, so their product is exact at that width and
 * nothing is rounded to form it. The sum of every applicable tuple is exact too.
 * The only roundings are the divisions that must be: a scalar's ratio of two
 * coordinates, and an interpolated delta's ratio of two distances. Each is
 * round-half-away-from-zero in 64 bits, which is the rule of design.md section
 * 5.2 and is FreeType's `FT_MulDiv`; what differs from FreeType is the width,
 * which it holds to sixteen bits and this library to twenty-four.
 *
 * Reference: OpenType Specification 1.9, "gvar - Glyph Variations Table".
 */

#include <ghoti.io/cutil/safemath.h>
#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/metrics.h>
#include <stdlib.h>
#include <string.h>
#include "../core/fixed.h"
#include "../reader/reader.h"
#include "../sfnt/sfnt.h"
#include "../tables/tables.h"
#include "gvar.h"
#include "var.h"

GFNT_Result gfnt_gvar_parse(const GFNT_Face * face, void * out, void * context,
    GFNT_Error * error) {
  (void)context;
  GFNT_Gvar * gvar = out;
  const GFNT_Fvar * fvar = NULL;
  GFNT_Reader reader;
  GFNT_Result result;
  uint16_t major = 0;
  uint16_t minor = 0;
  uint16_t axis_count = 0;
  uint16_t shared_count = 0;
  uint32_t shared_offset = 0;
  uint16_t glyph_count = 0;
  uint16_t flags = 0;
  uint32_t data_offset = 0;
  size_t offsets_bytes;

  memset(gvar, 0, sizeof *gvar);
  // Its axis count is checked against the design space's, so a face whose fvar
  // is refused has a gvar nobody can interpret either.
  result = gfnt_face_fvar(face, &fvar, error);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_face_table_reader(face, GFNT_TAG_GVAR, &reader, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (gfnt_read_u16(&reader, &major) != GFNT_OK
      || gfnt_read_u16(&reader, &minor) != GFNT_OK
      || gfnt_read_u16(&reader, &axis_count) != GFNT_OK
      || gfnt_read_u16(&reader, &shared_count) != GFNT_OK
      || gfnt_read_u32(&reader, &shared_offset) != GFNT_OK
      || gfnt_read_u16(&reader, &glyph_count) != GFNT_OK
      || gfnt_read_u16(&reader, &flags) != GFNT_OK
      || gfnt_read_u32(&reader, &data_offset) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_GVAR, 0,
        GFNT_GLYPH_NONE, "a gvar shorter than its own header");
  }
  (void)minor;
  if (major != 1) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_GVAR, 0,
        GFNT_GLYPH_NONE, "a gvar version other than 1.x, which is the only one "
        "the format has");
  }
  if (axis_count != fvar->axis_count) {
    // A tuple's coordinates are one per axis, so a table that counts a different
    // number reads every tuple at the wrong stride.
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_GVAR, 4,
        GFNT_GLYPH_NONE, "a gvar's axis count is not the fvar's");
  }
  gvar->long_offsets = (flags & 1u) != 0;
  offsets_bytes = ((size_t)glyph_count + 1u) * (gvar->long_offsets ? 4u : 2u);
  if (offsets_bytes > reader.length - GFNT_GVAR_HEADER_BYTES) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_GVAR,
        GFNT_GVAR_HEADER_BYTES, GFNT_GLYPH_NONE,
        "a gvar's glyph offset array runs past the table");
  }
  if ((size_t)shared_count * axis_count * 2u > reader.length
      || (shared_count > 0 && shared_offset > reader.length
          - (size_t)shared_count * axis_count * 2u)) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_GVAR, 8,
        GFNT_GLYPH_NONE, "a gvar's shared tuples run past the table");
  }
  if (data_offset > reader.length) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_GVAR, 16,
        GFNT_GLYPH_NONE, "a gvar's glyph data begins past the table");
  }
  gvar->axis_count = axis_count;
  gvar->shared_tuple_count = shared_count;
  gvar->shared_tuples_offset = shared_offset;
  gvar->glyph_count = glyph_count;
  gvar->data_offset = data_offset;
  return GFNT_OK;
}

GFNT_Result gfnt_face_gvar(const GFNT_Face * face, const GFNT_Gvar ** out_gvar,
    GFNT_Error * error) {
  GFNT_Gvar scratch;
  GFNT_Result result;

  if (!face || !out_gvar) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no face, or nowhere to put the table");
  }
  if (!gfnt_face_has_table(face, GFNT_TAG_GVAR)) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_GVAR, 0,
        GFNT_GLYPH_NONE, "the face has no gvar");
  }
  result = gfnt_table_cached(face, &((GFNT_Face *)face)->gvar_state,
      &((GFNT_Face *)face)->gvar, &scratch, sizeof scratch, gfnt_gvar_parse,
      NULL, NULL, error);
  if (result != GFNT_OK) {
    return result;
  }
  *out_gvar = &face->gvar;
  return GFNT_OK;
}

/** Where one glyph's variation data is: from the table's start, and how long. */
static GFNT_Result gfnt_gvar_glyph_range(const GFNT_Gvar * gvar,
    const GFNT_Reader * table, uint32_t glyph, size_t * out_start,
    size_t * out_length, GFNT_Error * error) {
  GFNT_Reader reader = *table;
  size_t start = 0;
  size_t end = 0;
  GFNT_Result result;

  if ((size_t)glyph >= gvar->glyph_count) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_GVAR, 0, glyph,
        "this glyph is past the glyph count the gvar states");
  }
  reader.error = error;
  if (gvar->long_offsets) {
    uint32_t low = 0;
    uint32_t high = 0;

    result = gfnt_reader_u32_at(&reader,
        GFNT_GVAR_HEADER_BYTES + (size_t)glyph * 4u, &low);
    if (result == GFNT_OK) {
      result = gfnt_reader_u32_at(&reader,
          GFNT_GVAR_HEADER_BYTES + ((size_t)glyph + 1u) * 4u, &high);
    }
    start = low;
    end = high;
  }
  else {
    uint16_t low = 0;
    uint16_t high = 0;

    // The short form stores half the offset, which is why it cannot express an
    // odd one.
    result = gfnt_reader_u16_at(&reader,
        GFNT_GVAR_HEADER_BYTES + (size_t)glyph * 2u, &low);
    if (result == GFNT_OK) {
      result = gfnt_reader_u16_at(&reader,
          GFNT_GVAR_HEADER_BYTES + ((size_t)glyph + 1u) * 2u, &high);
    }
    start = (size_t)low * 2u;
    end = (size_t)high * 2u;
  }
  if (result != GFNT_OK) {
    return result;
  }
  // M11: a backwards or overrunning entry condemns this glyph and not the font.
  if (end < start) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_GVAR, 0, glyph,
        "this glyph's gvar entry runs backwards");
  }
  if (start > table->length - gvar->data_offset
      || end > table->length - gvar->data_offset) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_GVAR, 0, glyph,
        "this glyph's gvar entry runs past the table");
  }
  *out_start = gvar->data_offset + start;
  *out_length = end - start;
  return GFNT_OK;
}

/**
 * A packed list of point numbers.
 *
 * @param total How many points a number may name, phantoms included.
 * @param out_numbers Receives the numbers, in the order read. Allocated by the
 *   caller with room for @p total.
 * @param out_count Receives how many, or @p total with *out_all for "every point".
 * @param out_all Receives whether the list is the empty-count shorthand for every
 *   point, in which case nothing is stored.
 */
GFNT_Result gfnt_tuple_points(GFNT_Reader * reader, size_t total,
    uint32_t * out_numbers, size_t * out_count, bool * out_all, GFNT_Tag table,
    uint32_t glyph, GFNT_Error * error) {
  uint8_t first = 0;
  size_t count = 0;
  size_t filled = 0;
  uint32_t previous = 0;
  GFNT_Result result = gfnt_read_u8(reader, &first);

  if (result != GFNT_OK) {
    return result;
  }
  if (first & 0x80u) {
    uint8_t low = 0;

    result = gfnt_read_u8(reader, &low);
    if (result != GFNT_OK) {
      return result;
    }
    count = ((size_t)(first & 0x7Fu) << 8) | low;
  }
  else {
    count = first;
  }
  *out_all = count == 0;
  if (count == 0) {
    *out_count = total;
    return GFNT_OK;
  }
  if (count > total) {
    // More numbers than there are points: some name one twice, or past the end.
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, table,
        gfnt_reader_tell(reader), glyph,
        "a gvar tuple names more points than the glyph has");
  }
  while (filled < count) {
    uint8_t control = 0;
    size_t run;
    bool words;

    result = gfnt_read_u8(reader, &control);
    if (result != GFNT_OK) {
      return result;
    }
    run = (size_t)(control & GFNT_GVAR_POINT_RUN_COUNT_MASK) + 1u;
    words = (control & GFNT_GVAR_POINTS_ARE_WORDS) != 0;
    if (run > count - filled) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, table,
          gfnt_reader_tell(reader), glyph,
          "a gvar point-number run is longer than the list it is in");
    }
    for (size_t i = 0; i < run; ++i) {
      uint32_t step = 0;

      if (words) {
        uint16_t value = 0;

        result = gfnt_read_u16(reader, &value);
        step = value;
      }
      else {
        uint8_t value = 0;

        result = gfnt_read_u8(reader, &value);
        step = value;
      }
      if (result != GFNT_OK) {
        return result;
      }
      // Each number is stored as the distance from the one before.
      previous += step;
      if ((size_t)previous >= total) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, table,
            gfnt_reader_tell(reader), glyph,
            "a gvar tuple names a point the glyph does not have");
      }
      out_numbers[filled] = previous;
      filled += 1;
    }
  }
  *out_count = count;
  return GFNT_OK;
}

/** One axis of packed deltas, @p count of them, into @p out. */
GFNT_Result gfnt_tuple_deltas(GFNT_Reader * reader, size_t count,
    int32_t * out, GFNT_Tag table, uint32_t glyph, GFNT_Error * error) {
  size_t filled = 0;

  while (filled < count) {
    uint8_t control = 0;
    size_t run;
    GFNT_Result result = gfnt_read_u8(reader, &control);

    if (result != GFNT_OK) {
      return result;
    }
    run = (size_t)(control & GFNT_GVAR_DELTA_RUN_COUNT_MASK) + 1u;
    if (run > count - filled) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, table,
          gfnt_reader_tell(reader), glyph,
          "a gvar delta run is longer than the list it is in");
    }
    if (control & GFNT_GVAR_DELTAS_ARE_ZERO) {
      for (size_t i = 0; i < run; ++i) {
        out[filled + i] = 0;
      }
    }
    else if (control & GFNT_GVAR_DELTAS_ARE_WORDS) {
      for (size_t i = 0; i < run; ++i) {
        int16_t value = 0;

        result = gfnt_read_s16(reader, &value);
        if (result != GFNT_OK) {
          return result;
        }
        out[filled + i] = value;
      }
    }
    else {
      for (size_t i = 0; i < run; ++i) {
        int8_t value = 0;

        result = gfnt_read_s8(reader, &value);
        if (result != GFNT_OK) {
          return result;
        }
        out[filled + i] = value;
      }
    }
    filled += run;
  }
  return GFNT_OK;
}

/**
 * How much of a tuple applies at this location, as a scalar with
 * ::GFNT_GVAR_FRACTION_BITS fractional bits.
 *
 * The product over the axes of how far inside the tuple's region the coordinate
 * is: one at the peak, falling linearly to zero at the region's edges.
 *
 * An axis whose peak is zero does not constrain the tuple, and a region the
 * format calls invalid - a start past the peak, an end before it, or one that
 * straddles zero with a peak that is not zero - is **ignored for that axis**
 * rather than refused, which is the specification's word for it and what every
 * reader does. The alternative is to condemn a glyph for a header a writer got
 * wrong that nobody else notices.
 */
int64_t gfnt_gvar_scalar(size_t axis_count, const GFNT_F2Dot14 * coords,
    size_t coord_count, const int16_t * peak, const int16_t * start,
    const int16_t * end) {
  int64_t scalar = (int64_t)1 << GFNT_GVAR_FRACTION_BITS;

  for (size_t i = 0; i < axis_count; ++i) {
    int32_t coordinate = i < coord_count ? coords[i] : 0;
    int32_t p = peak[i];
    int32_t low = start ? start[i] : (p < 0 ? p : 0);
    int32_t high = end ? end[i] : (p > 0 ? p : 0);

    if (p == 0 || low > p || p > high || (low < 0 && high > 0)) {
      continue;
    }
    if (coordinate == p) {
      continue;
    }
    if (coordinate <= low || coordinate >= high) {
      return 0;
    }
    if (coordinate < p) {
      scalar = gfnt_round_div(scalar * (coordinate - low), p - low);
    }
    else {
      scalar = gfnt_round_div(scalar * (high - coordinate), high - p);
    }
  }
  return scalar;
}

/**
 * Infer the deltas of the points a tuple does not name, contour by contour.
 *
 * "IUP", in the specification's term: a point between two that moved moves in
 * proportion, along each axis separately. A point at or beyond either end of the
 * span takes that end's delta, because there is nothing on its far side to
 * interpolate toward.
 *
 * @param have One flag per point: whether the tuple named it.
 * @param dx Deltas, **on entry holding the named points' and on exit
 *   every point's**. Same for @p dy.
 */
static void gfnt_gvar_infer(const GFNT_GvarPoints * points, const uint8_t * have,
    int64_t * dx, int64_t * dy) {
  size_t first = 0;

  for (size_t c = 0; c < points->contour_count; ++c) {
    size_t last = points->contour_ends[c];  // one past
    size_t referenced = 0;
    size_t anchor = 0;

    for (size_t i = first; i < last; ++i) {
      if (have[i]) {
        referenced += 1;
        anchor = i;
      }
    }
    if (referenced == 0) {
      first = last;
      continue;
    }
    if (referenced == 1) {
      // One point named: the whole contour moves with it.
      for (size_t i = first; i < last; ++i) {
        dx[i] = dx[anchor];
        dy[i] = dy[anchor];
      }
      first = last;
      continue;
    }
    // Walk the contour once, from the first named point round to it again, and
    // fill each run of unnamed points between a named pair.
    {
      size_t begin = first;
      size_t length = last - first;
      size_t previous = 0;

      while (begin < last && !have[begin]) {
        begin += 1;
      }
      previous = begin;
      for (size_t step = 1; step <= length; ++step) {
        size_t i = first + ((begin - first + step) % length);

        if (!have[i]) {
          continue;
        }
        // `previous` and `i` are consecutive named points, round the contour;
        // the points between them (in contour order, wrapping) are the run.
        for (size_t j = (previous - first + 1) % length;
            first + j != i; j = (j + 1) % length) {
          size_t point = first + j;

          for (int axis = 0; axis < 2; ++axis) {
            const int32_t * coord = axis == 0 ? points->x : points->y;
            int64_t * delta = axis == 0 ? dx : dy;
            int64_t c1 = coord[previous];
            int64_t c2 = coord[i];
            int64_t d1 = delta[previous];
            int64_t d2 = delta[i];
            int64_t v = coord[point];

            if (c1 > c2) {
              int64_t swap = c1;

              c1 = c2;
              c2 = swap;
              swap = d1;
              d1 = d2;
              d2 = swap;
            }
            if (c1 == c2) {
              // Both ends at one coordinate: they agree about it or they do not,
              // and when they do not there is no proportion to take.
              delta[point] = d1 == d2 ? d1 : 0;
            }
            else if (v <= c1) {
              delta[point] = d1;
            }
            else if (v >= c2) {
              delta[point] = d2;
            }
            else {
              delta[point] = d1 + gfnt_round_div((v - c1) * (d2 - d1), c2 - c1);
            }
          }
        }
        previous = i;
      }
    }
    first = last;
  }
}

GFNT_Result gfnt_gvar_glyph_deltas(const GFNT_Face * face, uint32_t glyph,
    const GFNT_F2Dot14 * coordinates, size_t coordinate_count,
    const GFNT_GvarPoints * points, int64_t * out_x, int64_t * out_y,
    GFNT_Error * error) {
  const GFNT_Gvar * gvar = NULL;
  GFNT_Reader table;
  GFNT_Reader data;
  GFNT_Result result;
  size_t start = 0;
  size_t length = 0;
  size_t total;
  uint16_t tuple_word = 0;
  uint16_t serialised = 0;
  size_t tuple_count;
  bool shared_points = false;
  uint32_t * shared = NULL;
  size_t shared_count = 0;
  bool shared_all = true;
  uint32_t * numbers = NULL;
  int32_t * tuple_dx = NULL;
  int32_t * tuple_dy = NULL;
  int64_t * acc_x = NULL;
  int64_t * acc_y = NULL;
  int64_t * spread_x = NULL;
  int64_t * spread_y = NULL;
  uint8_t * have = NULL;
  int16_t * peak = NULL;
  int16_t * low = NULL;
  int16_t * high = NULL;
  GFNT_Reader headers;
  GFNT_Reader serial;
  const GFNT_Allocator * allocator = face->allocator;

  total = points->count + GFNT_GVAR_PHANTOM_POINTS;
  memset(out_x, 0, total * sizeof *out_x);
  memset(out_y, 0, total * sizeof *out_y);

  result = gfnt_face_gvar(face, &gvar, error);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_face_table_reader(face, GFNT_TAG_GVAR, &table, error);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_gvar_glyph_range(gvar, &table, glyph, &start, &length, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (length == 0) {
    // A glyph with no variation data does not vary: every delta is zero, which is
    // what the zeroed outputs already say.
    return GFNT_OK;
  }
  result = gfnt_reader_sub(&table, start, length, &data);
  if (result != GFNT_OK) {
    return result;
  }
  data.error = error;
  if (gfnt_read_u16(&data, &tuple_word) != GFNT_OK
      || gfnt_read_u16(&data, &serialised) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_GVAR, start, glyph,
        "a glyph's gvar data is shorter than its own header");
  }
  tuple_count = tuple_word & GFNT_GVAR_COUNT_MASK;
  shared_points = (tuple_word & GFNT_GVAR_SHARED_POINT_NUMBERS) != 0;
  if (serialised > length) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_GVAR, start, glyph,
        "a glyph's gvar serialised data begins past its own end");
  }
  headers = data;
  result = gfnt_reader_sub(&data, serialised, GFNT_READER_REST, &serial);
  if (result != GFNT_OK) {
    return result;
  }
  serial.error = error;

  numbers = allocator->calloc_fn(allocator->ctx, total, sizeof *numbers);
  shared = allocator->calloc_fn(allocator->ctx, total, sizeof *shared);
  tuple_dx = allocator->calloc_fn(allocator->ctx, total, sizeof *tuple_dx);
  tuple_dy = allocator->calloc_fn(allocator->ctx, total, sizeof *tuple_dy);
  acc_x = allocator->calloc_fn(allocator->ctx, total, sizeof *acc_x);
  acc_y = allocator->calloc_fn(allocator->ctx, total, sizeof *acc_y);
  spread_x = allocator->calloc_fn(allocator->ctx, total, sizeof *spread_x);
  spread_y = allocator->calloc_fn(allocator->ctx, total, sizeof *spread_y);
  have = allocator->calloc_fn(allocator->ctx, total, sizeof *have);
  peak = allocator->calloc_fn(allocator->ctx, gvar->axis_count ? gvar->axis_count : 1u,
      sizeof *peak);
  low = allocator->calloc_fn(allocator->ctx, gvar->axis_count ? gvar->axis_count : 1u,
      sizeof *low);
  high = allocator->calloc_fn(allocator->ctx, gvar->axis_count ? gvar->axis_count : 1u,
      sizeof *high);
  if (!numbers || !shared || !tuple_dx || !tuple_dy || !acc_x || !acc_y
      || !spread_x || !spread_y || !have || !peak || !low || !high) {
    result = gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_GVAR, 0, glyph,
        "no memory for a glyph's variation deltas");
    goto done;
  }

  if (shared_points) {
    // The shared list is the first thing in the serialised data, once, and every
    // tuple that does not carry its own uses it.
    result = gfnt_tuple_points(&serial, total, shared, &shared_count, &shared_all,
        GFNT_TAG_GVAR, glyph, error);
    if (result != GFNT_OK) {
      goto done;
    }
  }

  for (size_t t = 0; t < tuple_count; ++t) {
    uint16_t size = 0;
    uint16_t index = 0;
    bool intermediate;
    bool private_points;
    int64_t scalar;
    GFNT_Reader tuple;
    size_t named;
    bool all;
    const uint32_t * list;

    if (gfnt_read_u16(&headers, &size) != GFNT_OK
        || gfnt_read_u16(&headers, &index) != GFNT_OK) {
      result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_GVAR,
          gfnt_reader_tell(&headers), glyph,
          "a gvar tuple header ends past the glyph's data");
      goto done;
    }
    intermediate = (index & GFNT_GVAR_INTERMEDIATE_REGION) != 0;
    private_points = (index & GFNT_GVAR_PRIVATE_POINT_NUMBERS) != 0;
    if (index & GFNT_GVAR_EMBEDDED_PEAK_TUPLE) {
      for (size_t a = 0; a < gvar->axis_count; ++a) {
        if (gfnt_read_f2dot14(&headers, &peak[a]) != GFNT_OK) {
          result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_GVAR,
              gfnt_reader_tell(&headers), glyph,
              "a gvar tuple header ends past the glyph's data");
          goto done;
        }
      }
    }
    else {
      GFNT_Reader shared_tuples;
      size_t which = index & GFNT_GVAR_TUPLE_INDEX_MASK;

      if (which >= gvar->shared_tuple_count) {
        result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_GVAR,
            gfnt_reader_tell(&headers), glyph,
            "a gvar tuple names a shared tuple the table does not have");
        goto done;
      }
      result = gfnt_reader_sub(&table, gvar->shared_tuples_offset
          + which * gvar->axis_count * 2u, gvar->axis_count * 2u,
          &shared_tuples);
      for (size_t a = 0; result == GFNT_OK && a < gvar->axis_count; ++a) {
        result = gfnt_read_f2dot14(&shared_tuples, &peak[a]);
      }
      if (result != GFNT_OK) {
        goto done;
      }
    }
    if (intermediate) {
      for (size_t a = 0; a < gvar->axis_count; ++a) {
        if (gfnt_read_f2dot14(&headers, &low[a]) != GFNT_OK) {
          result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_GVAR,
              gfnt_reader_tell(&headers), glyph,
              "a gvar tuple header ends past the glyph's data");
          goto done;
        }
      }
      for (size_t a = 0; a < gvar->axis_count; ++a) {
        if (gfnt_read_f2dot14(&headers, &high[a]) != GFNT_OK) {
          result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_GVAR,
              gfnt_reader_tell(&headers), glyph,
              "a gvar tuple header ends past the glyph's data");
          goto done;
        }
      }
    }

    // The tuple's own bytes, bounded by the size its header states. Taken before
    // the scalar is looked at, because the next tuple starts after this one's
    // data whether or not this one applies.
    result = gfnt_reader_sub(&serial, gfnt_reader_tell(&serial), size, &tuple);
    if (result != GFNT_OK) {
      goto done;
    }
    tuple.error = error;
    result = gfnt_reader_skip(&serial, size);
    if (result != GFNT_OK) {
      goto done;
    }
    scalar = gfnt_gvar_scalar(gvar->axis_count, coordinates, coordinate_count,
        peak, intermediate ? low : NULL, intermediate ? high : NULL);
    if (scalar == 0) {
      continue;
    }

    if (private_points) {
      result = gfnt_tuple_points(&tuple, total, numbers, &named, &all, GFNT_TAG_GVAR,
          glyph, error);
      if (result != GFNT_OK) {
        goto done;
      }
      list = numbers;
    }
    else {
      named = shared_points ? shared_count : total;
      all = !shared_points || shared_all;
      list = shared;
    }
    result = gfnt_tuple_deltas(&tuple, named, tuple_dx, GFNT_TAG_GVAR, glyph, error);
    if (result == GFNT_OK) {
      result = gfnt_tuple_deltas(&tuple, named, tuple_dy, GFNT_TAG_GVAR, glyph, error);
    }
    if (result != GFNT_OK) {
      goto done;
    }

    if (all) {
      // Every point is named, so there is nothing to infer and nothing to
      // overwrite: the deltas go straight into the sum, and the work is one pass
      // over the tuple's own deltas. A glyph with 4,095 tuples over 65,539 points
      // is 2.7e8 point-visits at best, and three more passes over all of them for
      // each tuple - clearing the flags, clearing the spread, adding it in - made
      // that several times worse. A list that names points explicitly keeps the
      // slower path, because a list may name one twice and the last delta stands,
      // which only a cleared spread can say.
      for (size_t i = 0; i < named; ++i) {
        acc_x[i] += (int64_t)tuple_dx[i] * scalar;
        acc_y[i] += (int64_t)tuple_dy[i] * scalar;
      }
      continue;
    }

    memset(have, 0, total);
    memset(spread_x, 0, total * sizeof *spread_x);
    memset(spread_y, 0, total * sizeof *spread_y);
    for (size_t i = 0; i < named; ++i) {
      size_t point = list[i];

      have[point] = 1;
      // A whole number of font units times a fraction is that fraction scaled,
      // exactly: nothing is rounded to scale a delta.
      spread_x[point] = (int64_t)tuple_dx[i] * scalar;
      spread_y[point] = (int64_t)tuple_dy[i] * scalar;
    }
    if (points->x && points->y && points->contour_ends) {
      gfnt_gvar_infer(points, have, spread_x, spread_y);
    }
    for (size_t i = 0; i < total; ++i) {
      acc_x[i] += spread_x[i];
      acc_y[i] += spread_y[i];
    }
  }
  for (size_t i = 0; i < total; ++i) {
    out_x[i] = acc_x[i];
    out_y[i] = acc_y[i];
  }
  result = GFNT_OK;

done:
  allocator->free_fn(allocator->ctx, numbers);
  allocator->free_fn(allocator->ctx, shared);
  allocator->free_fn(allocator->ctx, tuple_dx);
  allocator->free_fn(allocator->ctx, tuple_dy);
  allocator->free_fn(allocator->ctx, acc_x);
  allocator->free_fn(allocator->ctx, acc_y);
  allocator->free_fn(allocator->ctx, spread_x);
  allocator->free_fn(allocator->ctx, spread_y);
  allocator->free_fn(allocator->ctx, have);
  allocator->free_fn(allocator->ctx, peak);
  allocator->free_fn(allocator->ctx, low);
  allocator->free_fn(allocator->ctx, high);
  return result;
}
