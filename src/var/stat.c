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
 * The `STAT` reader. See `stat.h` in the public headers.
 *
 * Nothing is parsed ahead of a call: a lookup reads the header, then the one
 * record it was asked for, from the blob. A table is a few hundred bytes in the
 * fonts that carry one and the questions asked of it are one record each, so a
 * parsed copy would be an allocation and a lock to save a handful of reads.
 */

#include <ghoti.io/font/stat.h>
#include <ghoti.io/font/macros.h>
#include <stdio.h>
#include "../reader/reader.h"
#include "../sfnt/sfnt.h"
#include "../tables/tables.h"
#include "var.h"

#define GFNT_TAG_STAT GFNT_TAG('S', 'T', 'A', 'T')

// The header of version 1.0, and the longer one of 1.1 that adds the fallback.
#define GFNT_STAT_HEADER_10 18u
#define GFNT_STAT_HEADER_11 20u
// A design axis record's fixed part: a tag, a name ID and the ordering.
#define GFNT_STAT_AXIS_BYTES 8u

/** What the header says, once it has been read and its numbers made safe. */
typedef struct GFNT_StatHeader {
  uint16_t minor;
  uint16_t axis_size;
  uint16_t axis_count;
  uint32_t axes_offset;
  uint16_t value_count;
  uint32_t values_offset;
  uint16_t fallback;
} GFNT_StatHeader;

/** A 16.16 value at @p at; false when it is not inside the table. */
static bool gfnt_stat_fixed_at(const GFNT_Reader * table, size_t at,
    int32_t * out) {
  uint32_t raw = 0;

  if (gfnt_reader_u32_at(table, at, &raw) != GFNT_OK) {
    return false;
  }
  *out = (int32_t)raw;
  return true;
}

bool gfnt_face_has_stat(const GFNT_Face * face) {
  return face && gfnt_face_has_table(face, GFNT_TAG_STAT);
}

static GFNT_Result gfnt_stat_open(const GFNT_Face * face, GFNT_Reader * table,
    GFNT_StatHeader * header, GFNT_Error * error) {
  uint16_t major = 0;
  GFNT_Result result;

  gfnt_error_clear(error);
  if (!face) {
    return GFNT_ERR_INVALID;
  }
  if (!gfnt_face_has_table(face, GFNT_TAG_STAT)) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_STAT, 0,
        GFNT_GLYPH_NONE, "the face has no STAT");
  }
  result = gfnt_face_table_reader(face, GFNT_TAG_STAT, table, error);
  if (result != GFNT_OK) {
    return result;
  }
  table->error = error;
  header->fallback = 0;
  if (table->length < GFNT_STAT_HEADER_10
      || gfnt_reader_u16_at(table, 0, &major) != GFNT_OK
      || gfnt_reader_u16_at(table, 2, &header->minor) != GFNT_OK
      || gfnt_reader_u16_at(table, 4, &header->axis_size) != GFNT_OK
      || gfnt_reader_u16_at(table, 6, &header->axis_count) != GFNT_OK
      || gfnt_reader_u32_at(table, 8, &header->axes_offset) != GFNT_OK
      || gfnt_reader_u16_at(table, 12, &header->value_count) != GFNT_OK
      || gfnt_reader_u32_at(table, 14, &header->values_offset) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_STAT, 0,
        GFNT_GLYPH_NONE, "a STAT shorter than its own header");
  }
  if (major != 1) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_STAT, 0,
        GFNT_GLYPH_NONE, "a STAT version other than 1.x");
  }
  if (header->minor >= 1
      && (table->length < GFNT_STAT_HEADER_11
          || gfnt_reader_u16_at(table, 18, &header->fallback) != GFNT_OK)) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_STAT, 18,
        GFNT_GLYPH_NONE, "a STAT 1.1 with no room for its fallback name");
  }
  if (header->axis_count > 0 && header->axis_size < GFNT_STAT_AXIS_BYTES) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_STAT, 4,
        GFNT_GLYPH_NONE, "a STAT design axis record shorter than its fields");
  }
  return GFNT_OK;
}

GFNT_Result gfnt_face_stat_axis_count(const GFNT_Face * face, size_t * out_count,
    GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_StatHeader header;
  GFNT_Result result;

  if (!out_count) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_stat_open(face, &table, &header, error);
  if (result == GFNT_OK) {
    *out_count = header.axis_count;
  }
  return result;
}

GFNT_Result gfnt_face_stat_value_count(const GFNT_Face * face,
    size_t * out_count, GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_StatHeader header;
  GFNT_Result result;

  if (!out_count) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_stat_open(face, &table, &header, error);
  if (result == GFNT_OK) {
    *out_count = header.value_count;
  }
  return result;
}

GFNT_Result gfnt_face_stat_elided_fallback(const GFNT_Face * face,
    uint16_t * out_name_id, GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_StatHeader header;
  GFNT_Result result;

  if (!out_name_id) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_stat_open(face, &table, &header, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (header.minor < 1) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_STAT, 0,
        GFNT_GLYPH_NONE, "a STAT 1.0 has no elided fallback name");
  }
  *out_name_id = header.fallback;
  return GFNT_OK;
}

static GFNT_Result gfnt_stat_axis(const GFNT_Reader * table,
    const GFNT_StatHeader * header, size_t index, GFNT_StatAxis * out,
    GFNT_Error * error) {
  size_t at = (size_t)header->axes_offset + index * header->axis_size;
  uint32_t tag = 0;

  if (index >= header->axis_count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_STAT, 0,
        GFNT_GLYPH_NONE, "this STAT design axis index is past the count");
  }
  if (gfnt_reader_u32_at(table, at, &tag) != GFNT_OK
      || gfnt_reader_u16_at(table, at + 4, &out->name_id) != GFNT_OK
      || gfnt_reader_u16_at(table, at + 6, &out->ordering) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_STAT, at,
        GFNT_GLYPH_NONE, "a STAT design axis runs past the table");
  }
  out->tag = tag;
  return GFNT_OK;
}

GFNT_Result gfnt_face_stat_axis_at(const GFNT_Face * face, size_t index,
    GFNT_StatAxis * out_axis, GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_StatHeader header;
  GFNT_Result result;

  if (!out_axis) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_stat_open(face, &table, &header, error);
  if (result != GFNT_OK) {
    return result;
  }
  return gfnt_stat_axis(&table, &header, index, out_axis, error);
}

/** Where value @p index starts in the table, or the reason it cannot be read. */
static GFNT_Result gfnt_stat_value_offset(const GFNT_Reader * table,
    const GFNT_StatHeader * header, size_t index, size_t * out_at,
    GFNT_Error * error) {
  uint16_t relative = 0;

  if (index >= header->value_count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_STAT, 0,
        GFNT_GLYPH_NONE, "this STAT axis value index is past the count");
  }
  if (gfnt_reader_u16_at(table, (size_t)header->values_offset + 2u * index,
          &relative) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_STAT,
        (size_t)header->values_offset + 2u * index, GFNT_GLYPH_NONE,
        "the STAT axis value offsets run past the table");
  }
  *out_at = (size_t)header->values_offset + relative;
  return GFNT_OK;
}

static GFNT_Result gfnt_stat_value(const GFNT_Reader * table,
    const GFNT_StatHeader * header, size_t index, GFNT_StatValue * out,
    GFNT_Error * error) {
  GFNT_StatValue value = {0};
  uint16_t format = 0;
  uint16_t axis_or_count = 0;
  uint16_t last = 0;
  size_t at = 0;
  GFNT_Result result;
  bool fits;

  result = gfnt_stat_value_offset(table, header, index, &at, error);
  if (result != GFNT_OK) {
    return result;
  }
  fits = gfnt_reader_u16_at(table, at, &format) == GFNT_OK
      && gfnt_reader_u16_at(table, at + 2, &axis_or_count) == GFNT_OK
      && gfnt_reader_u16_at(table, at + 4, &value.flags) == GFNT_OK
      && gfnt_reader_u16_at(table, at + 6, &value.name_id) == GFNT_OK;
  if (fits) {
    switch (format) {
      case GFNT_STAT_VALUE:
        fits = gfnt_stat_fixed_at(table, at + 8, &value.value);
        break;
      case GFNT_STAT_RANGE:
        fits = gfnt_stat_fixed_at(table, at + 8, &value.value)
            && gfnt_stat_fixed_at(table, at + 12, &value.range_min)
            && gfnt_stat_fixed_at(table, at + 16, &value.range_max);
        break;
      case GFNT_STAT_LINKED:
        fits = gfnt_stat_fixed_at(table, at + 8, &value.value)
            && gfnt_stat_fixed_at(table, at + 12, &value.linked_value);
        break;
      case GFNT_STAT_MULTI:
        // The last pair's last byte is the one that has to be inside.
        fits = axis_or_count == 0
            || gfnt_reader_u16_at(table,
                   at + 8u + 6u * ((size_t)axis_or_count - 1u) + 4u, &last)
                == GFNT_OK;
        break;
      default:
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_STAT, at,
            GFNT_GLYPH_NONE, "a STAT axis value of a format other than 1 to 4");
    }
  }
  if (!fits) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_STAT, at,
        GFNT_GLYPH_NONE, "a STAT axis value runs past the table");
  }
  value.format = (GFNT_StatFormat)format;
  if (format == GFNT_STAT_MULTI) {
    value.pair_count = axis_or_count;
    // Each pair's axis is checked here, so that a value that comes back has axes
    // that exist and nobody who reads its pairs has to ask again.
    for (size_t p = 0; p < value.pair_count; ++p) {
      uint16_t axis = 0;

      if (gfnt_reader_u16_at(table, at + 8u + 6u * p, &axis) != GFNT_OK
          || axis >= header->axis_count) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_STAT,
            at + 8u + 6u * p, GFNT_GLYPH_NONE,
            "a STAT axis value names a design axis it lacks");
      }
    }
  }
  else {
    value.axis_index = axis_or_count;
    if (axis_or_count >= header->axis_count) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_STAT, at + 2,
          GFNT_GLYPH_NONE, "a STAT axis value names a design axis it lacks");
    }
  }
  *out = value;
  return GFNT_OK;
}

GFNT_Result gfnt_face_stat_value_at(const GFNT_Face * face, size_t index,
    GFNT_StatValue * out_value, GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_StatHeader header;
  GFNT_Result result;

  if (!out_value) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_stat_open(face, &table, &header, error);
  if (result != GFNT_OK) {
    return result;
  }
  return gfnt_stat_value(&table, &header, index, out_value, error);
}

static GFNT_Result gfnt_stat_pair(const GFNT_Reader * table,
    const GFNT_StatHeader * header, const GFNT_StatValue * value, size_t at,
    size_t pair_index, uint16_t * out_axis, GFNT_F16Dot16 * out_value,
    GFNT_Error * error) {
  size_t pair_at = at + 8u + 6u * pair_index;
  uint16_t axis = 0;
  int32_t coordinate = 0;

  if (value->format != GFNT_STAT_MULTI || pair_index >= value->pair_count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_STAT, 0,
        GFNT_GLYPH_NONE, "not a format 4 axis value, or a pair it does not have");
  }
  if (gfnt_reader_u16_at(table, pair_at, &axis) != GFNT_OK
      || !gfnt_stat_fixed_at(table, pair_at + 2, &coordinate)) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_STAT, pair_at,
        GFNT_GLYPH_NONE, "a STAT axis value runs past the table");
  }
  if (axis >= header->axis_count) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_STAT, pair_at,
        GFNT_GLYPH_NONE, "a STAT axis value names a design axis it lacks");
  }
  *out_axis = axis;
  *out_value = coordinate;
  return GFNT_OK;
}

GFNT_Result gfnt_face_stat_value_pair(const GFNT_Face * face,
    size_t value_index, size_t pair_index, uint16_t * out_axis_index,
    GFNT_F16Dot16 * out_value, GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_StatHeader header;
  GFNT_StatValue value;
  size_t at = 0;
  GFNT_Result result;

  if (!out_axis_index || !out_value) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_stat_open(face, &table, &header, error);
  if (result == GFNT_OK) {
    result = gfnt_stat_value(&table, &header, value_index, &value, error);
  }
  if (result == GFNT_OK) {
    result = gfnt_stat_value_offset(&table, &header, value_index, &at, error);
  }
  if (result != GFNT_OK) {
    return result;
  }
  return gfnt_stat_pair(&table, &header, &value, at, pair_index, out_axis_index,
      out_value, error);
}

/** The user coordinate of the face's `fvar` axis called @p tag; false if none. */
static bool gfnt_stat_coordinate(const GFNT_Fvar * fvar, GFNT_Tag tag,
    const GFNT_F16Dot16 * user, size_t count, int32_t * out) {
  for (size_t i = 0; i < fvar->axis_count; ++i) {
    if (fvar->axes[i].tag == tag) {
      *out = i < count ? user[i] : fvar->axes[i].def;
      return true;
    }
  }
  return false;
}

/** Whether the design axis @p index is at @p want (or in a range), at the location. */
static GFNT_Result gfnt_stat_axis_coordinate(const GFNT_Reader * table,
    const GFNT_StatHeader * header, const GFNT_Fvar * fvar,
    const GFNT_F16Dot16 * user, size_t count, uint16_t axis_index,
    bool * out_found, int32_t * out_coordinate, GFNT_Error * error) {
  GFNT_StatAxis axis;
  GFNT_Result result = gfnt_stat_axis(table, header, axis_index, &axis, error);

  if (result != GFNT_OK) {
    return result;
  }
  *out_found = gfnt_stat_coordinate(fvar, axis.tag, user, count,
      out_coordinate);
  return GFNT_OK;
}

GFNT_Result gfnt_face_stat_match(const GFNT_Face * face,
    const GFNT_F16Dot16 * user_coordinates, size_t count, size_t * out_values,
    size_t capacity, size_t * out_count, GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_StatHeader header;
  const GFNT_Fvar * fvar = NULL;
  size_t matched = 0;
  GFNT_Result result;

  if (!out_count || (count > 0 && !user_coordinates)
      || (capacity > 0 && !out_values)) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_stat_open(face, &table, &header, error);
  if (result == GFNT_OK) {
    result = gfnt_face_fvar(face, &fvar, error);
  }
  if (result != GFNT_OK) {
    return result;
  }
  if (count > fvar->axis_count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_FVAR, 0,
        GFNT_GLYPH_NONE, "more coordinates than the face has axes");
  }
  for (size_t v = 0; v < header.value_count; ++v) {
    GFNT_StatValue value;
    bool match = true;
    bool found = false;
    int32_t coordinate = 0;

    result = gfnt_stat_value(&table, &header, v, &value, error);
    if (result != GFNT_OK) {
      return result;
    }
    if (value.format == GFNT_STAT_MULTI) {
      size_t at = 0;

      result = gfnt_stat_value_offset(&table, &header, v, &at, error);
      for (size_t p = 0; result == GFNT_OK && match && p < value.pair_count;
          ++p) {
        uint16_t axis = 0;
        int32_t want = 0;

        result = gfnt_stat_pair(&table, &header, &value, at, p, &axis, &want,
            error);
        if (result == GFNT_OK) {
          result = gfnt_stat_axis_coordinate(&table, &header, fvar,
              user_coordinates, count, axis, &found, &coordinate, error);
        }
        match = result == GFNT_OK && found && coordinate == want;
      }
      if (result != GFNT_OK) {
        return result;
      }
      // A value that states no pairs names no place, so it matches nothing.
      match = match && value.pair_count > 0;
    }
    else {
      result = gfnt_stat_axis_coordinate(&table, &header, fvar,
          user_coordinates, count, value.axis_index, &found, &coordinate,
          error);
      if (result != GFNT_OK) {
        return result;
      }
      if (value.format == GFNT_STAT_RANGE) {
        match = found && coordinate >= value.range_min
            && coordinate <= value.range_max;
      }
      else {
        match = found && coordinate == value.value;
      }
    }
    if (match) {
      if (matched < capacity) {
        out_values[matched] = v;
      }
      matched += 1;
    }
  }
  *out_count = matched;
  return GFNT_OK;
}

GFNT_Result gfnt_face_stat_dump(const GFNT_Face * face, FILE * out) {
  GFNT_Reader table;
  GFNT_StatHeader header;
  GFNT_Result result;

  if (!out) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_stat_open(face, &table, &header, NULL);
  if (result != GFNT_OK) {
    return result;
  }
  if (fprintf(out, "STAT: version 1.%u, %u axes, %u values, fallback %u\n",
          (unsigned)header.minor, (unsigned)header.axis_count,
          (unsigned)header.value_count, (unsigned)header.fallback) < 0) {
    return GFNT_ERR_IO;
  }
  for (size_t i = 0; i < header.axis_count; ++i) {
    GFNT_StatAxis axis;
    char tag[5];

    result = gfnt_stat_axis(&table, &header, i, &axis, NULL);
    if (result != GFNT_OK) {
      return result;
    }
    gfnt_tag_string(axis.tag, tag);
    if (fprintf(out, "STAT axis %zu: '%s' name %u ordering %u\n", i, tag,
            (unsigned)axis.name_id, (unsigned)axis.ordering) < 0) {
      return GFNT_ERR_IO;
    }
  }
  for (size_t i = 0; i < header.value_count; ++i) {
    GFNT_StatValue value;
    size_t at = 0;

    result = gfnt_stat_value(&table, &header, i, &value, NULL);
    if (result == GFNT_OK) {
      result = gfnt_stat_value_offset(&table, &header, i, &at, NULL);
    }
    if (result != GFNT_OK) {
      return result;
    }
    if (fprintf(out, "STAT value %zu: format %d flags 0x%04X name %u", i,
            (int)value.format, (unsigned)value.flags,
            (unsigned)value.name_id) < 0) {
      return GFNT_ERR_IO;
    }
    if (value.format == GFNT_STAT_MULTI) {
      for (size_t p = 0; p < value.pair_count; ++p) {
        uint16_t axis = 0;
        int32_t coordinate = 0;

        result = gfnt_stat_pair(&table, &header, &value, at, p, &axis,
            &coordinate, NULL);
        if (result != GFNT_OK) {
          return result;
        }
        if (fprintf(out, " %u:%d", (unsigned)axis, (int)coordinate) < 0) {
          return GFNT_ERR_IO;
        }
      }
    }
    else if (fprintf(out, " axis %u value %d", (unsigned)value.axis_index,
                 (int)value.value) < 0
        || (value.format == GFNT_STAT_RANGE
            && fprintf(out, " min %d max %d", (int)value.range_min,
                   (int)value.range_max) < 0)
        || (value.format == GFNT_STAT_LINKED
            && fprintf(out, " linked %d", (int)value.linked_value) < 0)) {
      return GFNT_ERR_IO;
    }
    if (fprintf(out, "\n") < 0) {
      return GFNT_ERR_IO;
    }
  }
  return GFNT_OK;
}
