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
 * The `HVAR` and `MVAR` readers. See `metvar.h`.
 */

#include <ghoti.io/font/macros.h>
#include "../reader/reader.h"
#include "../sfnt/sfnt.h"
#include "../tables/tables.h"
#include "ivs.h"
#include "metvar.h"

// The bytes of an `HVAR` header, and of the part of an `MVAR`'s before its
// records.
#define GFNT_HVAR_HEADER_BYTES 20u
#define GFNT_MVAR_HEADER_BYTES 12u
// A value record is a tag and two indices; a table may state a longer one.
#define GFNT_MVAR_RECORD_BYTES 8u

GFNT_Result gfnt_hvar_delta(const GFNT_Face * face, uint32_t glyph,
    GFNT_HvarField field, const GFNT_F2Dot14 * coordinates,
    size_t coordinate_count, int64_t * out_delta, GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_Reader sub;
  uint16_t major = 0;
  uint32_t store_offset = 0;
  uint32_t map_offset = 0;
  uint32_t outer = 0;
  uint32_t inner = glyph;
  GFNT_Result result;

  if (!face || !out_delta) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, glyph,
        "no face, or nowhere to put the delta");
  }
  if (!gfnt_face_has_table(face, GFNT_TAG_HVAR)) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_HVAR, 0, glyph,
        "the face has no HVAR");
  }
  result = gfnt_face_table_reader(face, GFNT_TAG_HVAR, &table, error);
  if (result != GFNT_OK) {
    return result;
  }
  table.error = error;
  if (table.length < GFNT_HVAR_HEADER_BYTES
      || gfnt_reader_u16_at(&table, 0, &major) != GFNT_OK
      || gfnt_reader_u32_at(&table, 4, &store_offset) != GFNT_OK
      || gfnt_reader_u32_at(&table, field == GFNT_HVAR_ADVANCE ? 8u : 12u,
          &map_offset) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_HVAR, 0, glyph,
        "an HVAR shorter than its own header");
  }
  if (major != 1) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_HVAR, 0, glyph,
        "an HVAR version other than 1.x");
  }
  if (field == GFNT_HVAR_LEFT_BEARING && map_offset == 0) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_HVAR, 12, glyph,
        "this HVAR has no left side bearing mapping, so a bearing at a location "
        "follows the outline and not the table");
  }
  if (map_offset != 0) {
    result = gfnt_reader_sub(&table, map_offset, GFNT_READER_REST, &sub);
    if (result != GFNT_OK) {
      return result;
    }
    result = gfnt_ivs_map(&sub, GFNT_TAG_HVAR, glyph, &outer, &inner, error);
    if (result != GFNT_OK) {
      return result;
    }
  }
  // With no advance mapping the glyph number is the inner index and the outer is
  // zero, which is what `outer` and `inner` were initialised to.
  if (store_offset == 0) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_HVAR, 4, glyph,
        "an HVAR with no item variation store");
  }
  result = gfnt_reader_sub(&table, store_offset, GFNT_READER_REST, &sub);
  if (result != GFNT_OK) {
    return result;
  }
  return gfnt_ivs_delta(face, &sub, GFNT_TAG_HVAR, coordinates,
      coordinate_count, outer, inner, out_delta, error);
}

GFNT_Result gfnt_vvar_delta(const GFNT_Face * face, uint32_t glyph,
    GFNT_VvarField field, const GFNT_F2Dot14 * coordinates,
    size_t coordinate_count, int64_t * out_delta, GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_Reader sub;
  uint16_t major = 0;
  uint32_t store_offset = 0;
  uint32_t map_offset = 0;
  uint32_t outer = 0;
  uint32_t inner = glyph;
  GFNT_Result result;

  if (!face || !out_delta) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, glyph,
        "no face, or nowhere to put the delta");
  }
  if (!gfnt_face_has_table(face, GFNT_TAG_VVAR)) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_VVAR, 0, glyph,
        "the face has no VVAR");
  }
  result = gfnt_face_table_reader(face, GFNT_TAG_VVAR, &table, error);
  if (result != GFNT_OK) {
    return result;
  }
  table.error = error;
  if (table.length < 24u
      || gfnt_reader_u16_at(&table, 0, &major) != GFNT_OK
      || gfnt_reader_u32_at(&table, 4, &store_offset) != GFNT_OK
      || gfnt_reader_u32_at(&table, field == GFNT_VVAR_ADVANCE ? 8u : 20u,
          &map_offset) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_VVAR, 0, glyph,
        "a VVAR shorter than its own header");
  }
  if (major != 1) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_VVAR, 0, glyph,
        "a VVAR version other than 1.x");
  }
  if (field == GFNT_VVAR_ORIGIN && map_offset == 0) {
    *out_delta = 0;
    return GFNT_OK;
  }
  if (map_offset != 0) {
    result = gfnt_reader_sub(&table, map_offset, GFNT_READER_REST, &sub);
    if (result != GFNT_OK) {
      return result;
    }
    result = gfnt_ivs_map(&sub, GFNT_TAG_VVAR, glyph, &outer, &inner, error);
    if (result != GFNT_OK) {
      return result;
    }
  }
  if (store_offset == 0) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_VVAR, 4, glyph,
        "a VVAR with no item variation store");
  }
  result = gfnt_reader_sub(&table, store_offset, GFNT_READER_REST, &sub);
  if (result != GFNT_OK) {
    return result;
  }
  return gfnt_ivs_delta(face, &sub, GFNT_TAG_VVAR, coordinates,
      coordinate_count, outer, inner, out_delta, error);
}

GFNT_Result gfnt_mvar_delta(const GFNT_Face * face, GFNT_Tag tag,
    const GFNT_F2Dot14 * coordinates, size_t coordinate_count,
    int64_t * out_delta, GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_Reader sub;
  uint16_t major = 0;
  uint16_t record_size = 0;
  uint16_t record_count = 0;
  uint16_t store_offset = 0;
  GFNT_Result result;

  if (!face || !out_delta) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no face, or nowhere to put the delta");
  }
  *out_delta = 0;
  if (!gfnt_face_has_table(face, GFNT_TAG_MVAR)) {
    return GFNT_OK;
  }
  result = gfnt_face_table_reader(face, GFNT_TAG_MVAR, &table, error);
  if (result != GFNT_OK) {
    return result;
  }
  table.error = error;
  if (table.length < GFNT_MVAR_HEADER_BYTES
      || gfnt_reader_u16_at(&table, 0, &major) != GFNT_OK
      || gfnt_reader_u16_at(&table, 6, &record_size) != GFNT_OK
      || gfnt_reader_u16_at(&table, 8, &record_count) != GFNT_OK
      || gfnt_reader_u16_at(&table, 10, &store_offset) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_MVAR, 0,
        GFNT_GLYPH_NONE, "an MVAR shorter than its own header");
  }
  if (major != 1) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_MVAR, 0,
        GFNT_GLYPH_NONE, "an MVAR version other than 1.x");
  }
  if (record_count == 0) {
    return GFNT_OK;
  }
  if (record_size < GFNT_MVAR_RECORD_BYTES) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_MVAR, 6,
        GFNT_GLYPH_NONE, "an MVAR's value records are shorter than a record");
  }
  for (size_t i = 0; i < record_count; ++i) {
    size_t at = GFNT_MVAR_HEADER_BYTES + i * (size_t)record_size;
    uint32_t record_tag = 0;
    uint16_t outer = 0;
    uint16_t inner = 0;

    if (gfnt_reader_u32_at(&table, at, &record_tag) != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_MVAR, at,
          GFNT_GLYPH_NONE, "an MVAR's value records run past the table");
    }
    if (record_tag != tag) {
      continue;
    }
    if (gfnt_reader_u16_at(&table, at + 4u, &outer) != GFNT_OK
        || gfnt_reader_u16_at(&table, at + 6u, &inner) != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_MVAR, at,
          GFNT_GLYPH_NONE, "an MVAR's value records run past the table");
    }
    if (store_offset == 0) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_MVAR, 10,
          GFNT_GLYPH_NONE, "an MVAR with records and no item variation store");
    }
    result = gfnt_reader_sub(&table, store_offset, GFNT_READER_REST, &sub);
    if (result != GFNT_OK) {
      return result;
    }
    return gfnt_ivs_delta(face, &sub, GFNT_TAG_MVAR, coordinates,
        coordinate_count, outer, inner, out_delta, error);
  }
  return GFNT_OK;
}
