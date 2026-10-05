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
 * Item variation stores and delta-set index maps, read in place. See `ivs.h`.
 *
 * The arithmetic is `gvar`'s: a delta is a whole number of font units, a
 * region's scalar carries ::GFNT_GVAR_FRACTION_BITS fractional bits, and their
 * product is exact. The sum is exact too and is rounded **once**, by the caller,
 * to the integer a metric is. FreeType rounds each region's contribution on its
 * own, so the two can differ by up to a unit in a store with many regions; the
 * differential says how often.
 */

#include <ghoti.io/font/macros.h>
#include <stdlib.h>
#include "../sfnt/sfnt.h"
#include "gvar.h"
#include "ivs.h"
#include "var.h"

// Regions are copied into arrays on the stack up to this many axes; a font with
// more takes an allocation per lookup, which no real one does.
#define GFNT_IVS_STACK_AXES 16u

static GFNT_Result gfnt_ivs_corrupt(GFNT_Error * error, GFNT_Tag table,
    size_t offset, const char * message) {
  return gfnt_error_set(error, GFNT_ERR_CORRUPT, table, offset,
      GFNT_GLYPH_NONE, message);
}

GFNT_Result gfnt_ivs_map(const GFNT_Reader * map, GFNT_Tag table,
    uint32_t index, uint32_t * out_outer, uint32_t * out_inner,
    GFNT_Error * error) {
  GFNT_Reader reader = *map;
  uint8_t format = 0;
  uint8_t entry_format = 0;
  uint32_t count = 0;
  size_t data;
  size_t entry_bytes;
  unsigned inner_bits;
  uint32_t value = 0;
  GFNT_Result result;

  reader.error = error;
  result = gfnt_reader_u8_at(&reader, 0, &format);
  if (result == GFNT_OK) {
    result = gfnt_reader_u8_at(&reader, 1, &entry_format);
  }
  if (result != GFNT_OK) {
    return result;
  }
  if (format == 0) {
    uint16_t narrow = 0;

    result = gfnt_reader_u16_at(&reader, 2, &narrow);
    count = narrow;
    data = 4;
  }
  else if (format == 1) {
    result = gfnt_reader_u32_at(&reader, 2, &count);
    data = 6;
  }
  else {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, table, 0,
        GFNT_GLYPH_NONE, "a delta-set index map format other than 0 and 1");
  }
  if (result != GFNT_OK) {
    return result;
  }
  if (count == 0) {
    return gfnt_ivs_corrupt(error, table, 2,
        "a delta-set index map with no entries");
  }
  if ((entry_format & 0xC0u) != 0) {
    return gfnt_ivs_corrupt(error, table, 1,
        "a delta-set index map's entry format sets bits the format reserves");
  }
  entry_bytes = (size_t)((entry_format >> 4) & 3u) + 1u;
  inner_bits = (unsigned)(entry_format & 0x0Fu) + 1u;
  if (index >= count) {
    index = count - 1u;
  }
  for (size_t i = 0; i < entry_bytes; ++i) {
    uint8_t byte = 0;

    result = gfnt_reader_u8_at(&reader, data + (size_t)index * entry_bytes + i,
        &byte);
    if (result != GFNT_OK) {
      return result;
    }
    value = (value << 8) | byte;
  }
  *out_outer = value >> inner_bits;
  *out_inner = value & ((1u << inner_bits) - 1u);
  return GFNT_OK;
}

/**
 * The scalar of one region of a store's region list, at a location.
 *
 * @param region A buffer of three int16 per axis, which this fills: peaks, then
 *   starts, then ends, one array per field because that is what the scalar takes.
 */
static GFNT_Result gfnt_ivs_region_scalar(const GFNT_Reader * reader,
    GFNT_Tag table, size_t list_offset, size_t axes, uint16_t region_index,
    int16_t * region, const GFNT_F2Dot14 * coordinates,
    size_t coordinate_count, int64_t * out_scalar, GFNT_Error * error) {
  for (size_t a = 0; a < axes; ++a) {
    size_t at_axis = list_offset + 4u + ((size_t)region_index * axes + a) * 6u;
    int16_t triple[3] = { 0, 0, 0 };

    for (size_t t = 0; t < 3; ++t) {
      GFNT_Result result = gfnt_reader_s16_at(reader, at_axis + t * 2u,
          &triple[t]);

      if (result != GFNT_OK) {
        return gfnt_ivs_corrupt(error, table, at_axis,
            "an item variation store's regions run past it");
      }
    }
    // start, peak, end in the file; one array per field in the scalar.
    region[a] = triple[1];
    region[axes + a] = triple[0];
    region[2u * axes + a] = triple[2];
  }
  *out_scalar = gfnt_gvar_scalar(axes, coordinates, coordinate_count, region,
      region + axes, region + 2u * axes);
  return GFNT_OK;
}

GFNT_Result gfnt_ivs_delta(const GFNT_Face * face, const GFNT_Reader * store,
    GFNT_Tag table, const GFNT_F2Dot14 * coordinates, size_t coordinate_count,
    uint32_t outer, uint32_t inner, int64_t * out_delta, GFNT_Error * error) {
  const GFNT_Allocator * allocator = face->allocator;
  const GFNT_Fvar * fvar = NULL;
  GFNT_Reader reader = *store;
  int16_t stack_region[3u * GFNT_IVS_STACK_AXES];
  int16_t * region = stack_region;
  uint16_t format = 0;
  uint32_t list_offset = 0;
  uint16_t data_count = 0;
  uint32_t data_offset = 0;
  uint16_t region_axes = 0;
  uint16_t region_count = 0;
  uint16_t item_count = 0;
  uint16_t word_field = 0;
  uint16_t index_count = 0;
  size_t words;
  bool wide;
  size_t row_bytes;
  size_t row;
  size_t axes;
  int64_t sum = 0;
  GFNT_Result result;

  reader.error = error;
  result = gfnt_face_fvar(face, &fvar, error);
  if (result != GFNT_OK) {
    return result;
  }
  axes = fvar->axis_count;

  if (gfnt_reader_u16_at(&reader, 0, &format) != GFNT_OK
      || gfnt_reader_u32_at(&reader, 2, &list_offset) != GFNT_OK
      || gfnt_reader_u16_at(&reader, 6, &data_count) != GFNT_OK) {
    return gfnt_ivs_corrupt(error, table, 0,
        "an item variation store shorter than its own header");
  }
  if (format != 1) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, table, 0,
        GFNT_GLYPH_NONE, "an item variation store format other than 1");
  }
  if (outer >= data_count) {
    return gfnt_ivs_corrupt(error, table, 6,
        "an index names an item variation data the store does not have");
  }
  if (gfnt_reader_u32_at(&reader, 8u + 4u * (size_t)outer, &data_offset)
          != GFNT_OK
      || gfnt_reader_u16_at(&reader, list_offset, &region_axes) != GFNT_OK
      || gfnt_reader_u16_at(&reader, (size_t)list_offset + 2u, &region_count)
          != GFNT_OK) {
    return gfnt_ivs_corrupt(error, table, list_offset,
        "an item variation store's region list or data offset runs past it");
  }
  if (region_axes != axes) {
    // A region is one triple per axis, so a list that counts a different number
    // reads every region at the wrong stride.
    return gfnt_ivs_corrupt(error, table, list_offset,
        "an item variation store's axis count is not fvar's");
  }
  if (gfnt_reader_u16_at(&reader, data_offset, &item_count) != GFNT_OK
      || gfnt_reader_u16_at(&reader, (size_t)data_offset + 2u, &word_field)
          != GFNT_OK
      || gfnt_reader_u16_at(&reader, (size_t)data_offset + 4u, &index_count)
          != GFNT_OK) {
    return gfnt_ivs_corrupt(error, table, data_offset,
        "an item variation data shorter than its own header");
  }
  if (inner >= item_count) {
    return gfnt_ivs_corrupt(error, table, data_offset,
        "an index names an item the variation data does not have");
  }
  wide = (word_field & 0x8000u) != 0;
  words = word_field & 0x7FFFu;
  if (words > index_count) {
    return gfnt_ivs_corrupt(error, table, (size_t)data_offset + 2u,
        "an item variation data has more word deltas than regions");
  }
  row_bytes = words * (wide ? 4u : 2u)
      + ((size_t)index_count - words) * (wide ? 2u : 1u);
  row = (size_t)data_offset + 6u + 2u * (size_t)index_count
      + (size_t)inner * row_bytes;

  if (axes > GFNT_IVS_STACK_AXES) {
    region = allocator->calloc_fn(allocator->ctx, axes * 3u, sizeof *region);
    if (!region) {
      return gfnt_error_set(error, GFNT_ERR_OOM, table, 0, GFNT_GLYPH_NONE,
          "no memory for a variation region");
    }
  }
  for (size_t k = 0; k < index_count; ++k) {
    uint16_t region_index = 0;
    int32_t delta = 0;
    size_t at = row + (k < words ? k * (wide ? 4u : 2u)
        : words * (wide ? 4u : 2u) + (k - words) * (wide ? 2u : 1u));
    int64_t scalar;
    int64_t term;

    result = gfnt_reader_u16_at(&reader, (size_t)data_offset + 6u + 2u * k,
        &region_index);
    if (result != GFNT_OK) {
      goto done;
    }
    if (region_index >= region_count) {
      result = gfnt_ivs_corrupt(error, table, (size_t)data_offset + 6u + 2u * k,
          "an item variation data names a region the list does not have");
      goto done;
    }
    if (k < words && wide) {
      uint32_t raw = 0;

      result = gfnt_reader_u32_at(&reader, at, &raw);
      delta = (int32_t)raw;
    }
    else if (k < words || wide) {
      int16_t narrow = 0;

      result = gfnt_reader_s16_at(&reader, at, &narrow);
      delta = narrow;
    }
    else {
      uint8_t byte = 0;

      result = gfnt_reader_u8_at(&reader, at, &byte);
      delta = (int8_t)byte;
    }
    if (result != GFNT_OK) {
      result = gfnt_ivs_corrupt(error, table, at,
          "an item variation data's rows run past the store");
      goto done;
    }
    if (delta == 0) {
      continue;
    }
    result = gfnt_ivs_region_scalar(&reader, table, list_offset, axes,
        region_index, region, coordinates, coordinate_count, &scalar, error);
    if (result != GFNT_OK) {
      goto done;
    }
    term = (int64_t)delta * scalar;  // at most 2^31 * 2^24
    if (__builtin_add_overflow(sum, term, &sum)) {
      result = gfnt_ivs_corrupt(error, table, row,
          "an item's deltas sum past what a delta can be");
      goto done;
    }
  }
  *out_delta = sum;
  result = GFNT_OK;
done:
  if (region != stack_region) {
    allocator->free_fn(allocator->ctx, region);
  }
  return result;
}

GFNT_Result gfnt_ivs_region_scalars(const GFNT_Face * face,
    const GFNT_Reader * store, GFNT_Tag table, const GFNT_F2Dot14 * coordinates,
    size_t coordinate_count, uint32_t data_index, int64_t * out_scalars,
    size_t capacity, size_t * out_count, GFNT_Error * error) {
  const GFNT_Allocator * allocator = face->allocator;
  const GFNT_Fvar * fvar = NULL;
  GFNT_Reader reader = *store;
  int16_t stack_region[3u * GFNT_IVS_STACK_AXES];
  int16_t * region = stack_region;
  uint16_t format = 0;
  uint32_t list_offset = 0;
  uint16_t data_count = 0;
  uint32_t data_offset = 0;
  uint16_t region_axes = 0;
  uint16_t region_count = 0;
  uint16_t index_count = 0;
  size_t axes;
  GFNT_Result result;

  reader.error = error;
  result = gfnt_face_fvar(face, &fvar, error);
  if (result != GFNT_OK) {
    return result;
  }
  axes = fvar->axis_count;
  if (gfnt_reader_u16_at(&reader, 0, &format) != GFNT_OK
      || gfnt_reader_u32_at(&reader, 2, &list_offset) != GFNT_OK
      || gfnt_reader_u16_at(&reader, 6, &data_count) != GFNT_OK) {
    return gfnt_ivs_corrupt(error, table, 0,
        "an item variation store shorter than its own header");
  }
  if (format != 1) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, table, 0,
        GFNT_GLYPH_NONE, "an item variation store format other than 1");
  }
  if (data_index >= data_count) {
    return gfnt_ivs_corrupt(error, table, 6,
        "an index names an item variation data the store does not have");
  }
  if (gfnt_reader_u32_at(&reader, 8u + 4u * (size_t)data_index, &data_offset)
          != GFNT_OK
      || gfnt_reader_u16_at(&reader, list_offset, &region_axes) != GFNT_OK
      || gfnt_reader_u16_at(&reader, (size_t)list_offset + 2u, &region_count)
          != GFNT_OK
      || gfnt_reader_u16_at(&reader, (size_t)data_offset + 4u, &index_count)
          != GFNT_OK) {
    return gfnt_ivs_corrupt(error, table, list_offset,
        "an item variation store's region list or data offset runs past it");
  }
  if (region_axes != axes) {
    return gfnt_ivs_corrupt(error, table, list_offset,
        "an item variation store's axis count is not fvar's");
  }
  if (index_count > capacity) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, table, data_offset,
        GFNT_GLYPH_NONE, "an item variation data with more regions than the "
        "caller made room for");
  }
  if (axes > GFNT_IVS_STACK_AXES) {
    region = allocator->calloc_fn(allocator->ctx, axes * 3u, sizeof *region);
    if (!region) {
      return gfnt_error_set(error, GFNT_ERR_OOM, table, 0, GFNT_GLYPH_NONE,
          "no memory for a variation region");
    }
  }
  for (size_t k = 0; k < index_count; ++k) {
    uint16_t region_index = 0;

    result = gfnt_reader_u16_at(&reader, (size_t)data_offset + 6u + 2u * k,
        &region_index);
    if (result != GFNT_OK) {
      result = gfnt_ivs_corrupt(error, table, (size_t)data_offset + 6u + 2u * k,
          "an item variation data's region indices run past the store");
      break;
    }
    if (region_index >= region_count) {
      result = gfnt_ivs_corrupt(error, table, (size_t)data_offset + 6u + 2u * k,
          "an item variation data names a region the list does not have");
      break;
    }
    result = gfnt_ivs_region_scalar(&reader, table, list_offset, axes,
        region_index, region, coordinates, coordinate_count, &out_scalars[k],
        error);
    if (result != GFNT_OK) {
      break;
    }
  }
  if (region != stack_region) {
    allocator->free_fn(allocator->ctx, region);
  }
  if (result == GFNT_OK) {
    *out_count = index_count;
  }
  return result;
}
