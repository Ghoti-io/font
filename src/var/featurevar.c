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
 * The `FeatureVariations` reader. See `featurevar.h` in the public headers.
 *
 * Read in place, like `STAT`: a lookup walks the offsets it needs and keeps
 * nothing. The one recursion is a condition tree, and it is bounded by depth
 * rather than trusted, because a table is free to point a condition at itself.
 */

#include <ghoti.io/font/featurevar.h>
#include <ghoti.io/font/macros.h>
#include <stdio.h>
#include <string.h>
#include "../reader/reader.h"
#include "../sfnt/sfnt.h"
#include "../tables/tables.h"

#define GFNT_TAG_GSUB GFNT_TAG('G', 'S', 'U', 'B')
#define GFNT_TAG_GPOS GFNT_TAG('G', 'P', 'O', 'S')

// A condition nested this deep is refused. The specification states no bound, and
// a font that needs sixteen levels of and/or/not has not been seen; what the bound
// is for is a table whose condition names itself.
#define GFNT_FEATUREVAR_MAX_DEPTH 16u

/** A layout table's FeatureVariations, once it has been found. */
typedef struct GFNT_FeatureVariations {
  GFNT_Reader table;       ///< The layout table.
  GFNT_Reader variations;  ///< Begins at the FeatureVariations table.
  uint32_t count;
} GFNT_FeatureVariations;

static GFNT_Result gfnt_featurevar_open(const GFNT_Face * face, GFNT_Tag tag,
    GFNT_FeatureVariations * out, GFNT_Error * error) {
  uint16_t major = 0;
  uint16_t minor = 0;
  uint32_t offset = 0;
  uint16_t variations_major = 0;
  GFNT_Result result;

  gfnt_error_clear(error);
  if (!face || (tag != GFNT_TAG_GSUB && tag != GFNT_TAG_GPOS)) {
    return GFNT_ERR_INVALID;
  }
  memset(out, 0, sizeof *out);
  if (!gfnt_face_has_table(face, tag)) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, tag, 0, GFNT_GLYPH_NONE,
        "the face has no such layout table");
  }
  result = gfnt_face_table_reader(face, tag, &out->table, error);
  if (result != GFNT_OK) {
    return result;
  }
  out->table.error = error;
  if (gfnt_reader_u16_at(&out->table, 0, &major) != GFNT_OK
      || gfnt_reader_u16_at(&out->table, 2, &minor) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, 0, GFNT_GLYPH_NONE,
        "a layout table shorter than its own version");
  }
  if (major != 1) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, tag, 0, GFNT_GLYPH_NONE,
        "a layout table version other than 1.x");
  }
  if (minor < 1) {
    return GFNT_OK;
  }
  if (gfnt_reader_u32_at(&out->table, 10, &offset) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, 10, GFNT_GLYPH_NONE,
        "a layout table 1.1 with no room for its FeatureVariations offset");
  }
  if (offset == 0) {
    return GFNT_OK;
  }
  result = gfnt_reader_sub(&out->table, offset, GFNT_READER_REST,
      &out->variations);
  if (result != GFNT_OK) {
    return result;
  }
  out->variations.error = error;
  if (gfnt_reader_u16_at(&out->variations, 0, &variations_major) != GFNT_OK
      || gfnt_reader_u32_at(&out->variations, 4, &out->count) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, offset,
        GFNT_GLYPH_NONE, "a FeatureVariations shorter than its own header");
  }
  if (variations_major != 1) {
    out->count = 0;
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, tag, offset,
        GFNT_GLYPH_NONE, "a FeatureVariations version other than 1.x");
  }
  // The records are eight bytes each and have to fit: a count that does not is a
  // table that lies about its own size, and every walk below trusts it.
  if (out->count > 0
      && gfnt_reader_u32_at(&out->variations,
             8u + 8u * ((size_t)out->count - 1u) + 4u, &offset) != GFNT_OK) {
    out->count = 0;
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, 8, GFNT_GLYPH_NONE,
        "the FeatureVariations records run past the table");
  }
  return GFNT_OK;
}

GFNT_Result gfnt_face_feature_variations_count(const GFNT_Face * face,
    GFNT_Tag table, size_t * out_count, GFNT_Error * error) {
  GFNT_FeatureVariations found;
  GFNT_Result result;

  if (!out_count) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_featurevar_open(face, table, &found, error);
  if (result == GFNT_OK) {
    *out_count = found.count;
  }
  return result;
}

/**
 * Whether the condition at @p at in @p parent holds at the location.
 *
 * @p parent is a reader the offset is relative to - the ConditionSet for a
 * condition in a set, the enclosing condition for one inside an and, or, not.
 */
static GFNT_Result gfnt_condition_holds(const GFNT_Reader * parent,
    size_t offset, GFNT_Tag tag, const GFNT_F2Dot14 * coordinates,
    size_t coordinate_count, unsigned depth, bool * out_holds,
    GFNT_Error * error) {
  GFNT_Reader condition;
  uint16_t format = 0;
  GFNT_Result result;

  if (depth > GFNT_FEATUREVAR_MAX_DEPTH) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, tag, offset, GFNT_GLYPH_NONE,
        "conditions nested deeper than this library follows");
  }
  result = gfnt_reader_sub(parent, offset, GFNT_READER_REST, &condition);
  if (result != GFNT_OK) {
    return result;
  }
  condition.error = error;
  if (gfnt_reader_u16_at(&condition, 0, &format) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, offset, GFNT_GLYPH_NONE,
        "a condition table cut short");
  }
  if (format == 1) {
    uint16_t axis = 0;
    int16_t low = 0;
    int16_t high = 0;
    int32_t value;

    if (gfnt_reader_u16_at(&condition, 2, &axis) != GFNT_OK
        || gfnt_reader_s16_at(&condition, 4, &low) != GFNT_OK
        || gfnt_reader_s16_at(&condition, 6, &high) != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, offset,
          GFNT_GLYPH_NONE, "a format 1 condition cut short");
    }
    value = axis < coordinate_count && coordinates ? coordinates[axis] : 0;
    *out_holds = low <= value && value <= high;
    return GFNT_OK;
  }
  if (format == 3 || format == 4) {
    uint8_t count = 0;
    bool all = true;
    bool any = false;

    if (gfnt_reader_u8_at(&condition, 2, &count) != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, offset,
          GFNT_GLYPH_NONE, "an and/or condition cut short");
    }
    for (size_t i = 0; i < count; ++i) {
      uint8_t bytes[3];
      bool holds = false;

      for (size_t k = 0; k < 3; ++k) {
        if (gfnt_reader_u8_at(&condition, 3u + 3u * i + k, &bytes[k])
            != GFNT_OK) {
          return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, offset,
              GFNT_GLYPH_NONE, "an and/or condition's offsets run past it");
        }
      }
      result = gfnt_condition_holds(&condition,
          ((size_t)bytes[0] << 16) | ((size_t)bytes[1] << 8) | bytes[2], tag,
          coordinates, coordinate_count, depth + 1, &holds, error);
      if (result != GFNT_OK) {
        return result;
      }
      all = all && holds;
      any = any || holds;
    }
    *out_holds = format == 3 ? all : any;
    return GFNT_OK;
  }
  if (format == 5) {
    uint8_t bytes[3];
    bool holds = false;

    for (size_t k = 0; k < 3; ++k) {
      if (gfnt_reader_u8_at(&condition, 2u + k, &bytes[k]) != GFNT_OK) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, offset,
            GFNT_GLYPH_NONE, "a not condition cut short");
      }
    }
    result = gfnt_condition_holds(&condition,
        ((size_t)bytes[0] << 16) | ((size_t)bytes[1] << 8) | bytes[2], tag,
        coordinates, coordinate_count, depth + 1, &holds, error);
    if (result != GFNT_OK) {
      return result;
    }
    *out_holds = !holds;
    return GFNT_OK;
  }
  return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, tag, offset,
      GFNT_GLYPH_NONE, format == 2
          ? "a format 2 condition, a value varied through GDEF, which this "
            "library does not evaluate"
          : "a condition of a format this library does not know");
}

/** The ConditionSet of record @p record, or none (an unconditional record). */
static GFNT_Result gfnt_featurevar_conditions(
    const GFNT_FeatureVariations * found, GFNT_Tag tag, size_t record,
    GFNT_Reader * out_set, uint16_t * out_count, GFNT_Error * error) {
  uint32_t offset = 0;
  GFNT_Result result;

  *out_count = 0;
  memset(out_set, 0, sizeof *out_set);
  if (gfnt_reader_u32_at(&found->variations, 8u + 8u * record, &offset)
      != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, 8, GFNT_GLYPH_NONE,
        "the FeatureVariations records run past the table");
  }
  if (offset == 0) {
    return GFNT_OK;
  }
  result = gfnt_reader_sub(&found->variations, offset, GFNT_READER_REST, out_set);
  if (result != GFNT_OK) {
    return result;
  }
  out_set->error = error;
  if (gfnt_reader_u16_at(out_set, 0, out_count) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, offset, GFNT_GLYPH_NONE,
        "a ConditionSet cut short");
  }
  return GFNT_OK;
}

/** The offset of condition @p index in a ConditionSet. */
static GFNT_Result gfnt_featurevar_condition_offset(const GFNT_Reader * set,
    GFNT_Tag tag, size_t index, uint32_t * out_offset, GFNT_Error * error) {
  if (gfnt_reader_u32_at(set, 2u + 4u * index, out_offset) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, 2u + 4u * index,
        GFNT_GLYPH_NONE, "a ConditionSet's offsets run past the table");
  }
  return GFNT_OK;
}

GFNT_Result gfnt_face_feature_variations_match(const GFNT_Face * face,
    GFNT_Tag table, const GFNT_F2Dot14 * coordinates, size_t coordinate_count,
    size_t * out_record, GFNT_Error * error) {
  GFNT_FeatureVariations found;
  GFNT_Result result;

  if (!out_record || (coordinate_count > 0 && !coordinates)) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_featurevar_open(face, table, &found, error);
  if (result != GFNT_OK) {
    return result;
  }
  *out_record = GFNT_FEATURE_VARIATIONS_NONE;
  for (size_t r = 0; r < found.count; ++r) {
    GFNT_Reader set;
    uint16_t count = 0;
    bool holds = true;

    result = gfnt_featurevar_conditions(&found, table, r, &set, &count, error);
    for (size_t c = 0; result == GFNT_OK && holds && c < count; ++c) {
      uint32_t offset = 0;

      result = gfnt_featurevar_condition_offset(&set, table, c, &offset, error);
      if (result == GFNT_OK) {
        result = gfnt_condition_holds(&set, offset, table, coordinates,
            coordinate_count, 0, &holds, error);
      }
    }
    if (result != GFNT_OK) {
      return result;
    }
    // The first record that holds is the one that applies, and the rest are not
    // read - which is the specification's rule and not an optimisation.
    if (holds) {
      *out_record = r;
      return GFNT_OK;
    }
  }
  return GFNT_OK;
}

/** The FeatureTableSubstitution of a record, and how many it substitutes. */
static GFNT_Result gfnt_featurevar_substitution(
    const GFNT_FeatureVariations * found, GFNT_Tag tag, size_t record,
    GFNT_Reader * out_table, uint16_t * out_count, GFNT_Error * error) {
  uint32_t offset = 0;
  uint16_t major = 0;
  GFNT_Result result;

  *out_count = 0;
  if (record >= found->count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, tag, 0, GFNT_GLYPH_NONE,
        "this FeatureVariations record index is past the count");
  }
  if (gfnt_reader_u32_at(&found->variations, 8u + 8u * record + 4u, &offset)
      != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, 8, GFNT_GLYPH_NONE,
        "the FeatureVariations records run past the table");
  }
  if (offset == 0) {
    return GFNT_OK;
  }
  result = gfnt_reader_sub(&found->variations, offset, GFNT_READER_REST,
      out_table);
  if (result != GFNT_OK) {
    return result;
  }
  out_table->error = error;
  if (gfnt_reader_u16_at(out_table, 0, &major) != GFNT_OK
      || gfnt_reader_u16_at(out_table, 4, out_count) != GFNT_OK) {
    *out_count = 0;
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, offset, GFNT_GLYPH_NONE,
        "a FeatureTableSubstitution cut short");
  }
  if (major != 1) {
    *out_count = 0;
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, tag, offset,
        GFNT_GLYPH_NONE, "a FeatureTableSubstitution version other than 1.x");
  }
  return GFNT_OK;
}

GFNT_Result gfnt_face_feature_substitution_count(const GFNT_Face * face,
    GFNT_Tag table, size_t record, size_t * out_count, GFNT_Error * error) {
  GFNT_FeatureVariations found;
  GFNT_Reader substitution;
  uint16_t count = 0;
  GFNT_Result result;

  if (!out_count) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_featurevar_open(face, table, &found, error);
  if (result == GFNT_OK) {
    result = gfnt_featurevar_substitution(&found, table, record, &substitution,
        &count, error);
  }
  if (result == GFNT_OK) {
    *out_count = count;
  }
  return result;
}

/** Where substitution @p index's feature table is, as a reader. */
static GFNT_Result gfnt_featurevar_feature(const GFNT_Reader * substitution,
    GFNT_Tag tag, uint16_t count, size_t index, uint16_t * out_feature_index,
    GFNT_Reader * out_feature, GFNT_Error * error) {
  uint32_t offset = 0;

  if (index >= count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, tag, 0, GFNT_GLYPH_NONE,
        "this substitution index is past the record's count");
  }
  if (gfnt_reader_u16_at(substitution, 6u + 6u * index, out_feature_index)
          != GFNT_OK
      || gfnt_reader_u32_at(substitution, 6u + 6u * index + 2u, &offset)
          != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, 6u + 6u * index,
        GFNT_GLYPH_NONE, "a FeatureTableSubstitution's records run past it");
  }
  return gfnt_reader_sub(substitution, offset, GFNT_READER_REST, out_feature);
}

GFNT_Result gfnt_face_feature_substitution_at(const GFNT_Face * face,
    GFNT_Tag table, size_t record, size_t index, uint16_t * out_feature_index,
    size_t * out_lookup_count, GFNT_Error * error) {
  GFNT_FeatureVariations found;
  GFNT_Reader substitution;
  GFNT_Reader feature;
  uint16_t count = 0;
  uint16_t feature_index = 0;
  uint16_t lookups = 0;
  GFNT_Result result;

  if (!out_feature_index || !out_lookup_count) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_featurevar_open(face, table, &found, error);
  if (result == GFNT_OK) {
    result = gfnt_featurevar_substitution(&found, table, record, &substitution,
        &count, error);
  }
  if (result == GFNT_OK) {
    result = gfnt_featurevar_feature(&substitution, table, count, index,
        &feature_index, &feature, error);
  }
  if (result != GFNT_OK) {
    return result;
  }
  feature.error = error;
  if (gfnt_reader_u16_at(&feature, 2, &lookups) != GFNT_OK
      || (lookups > 0
          && gfnt_reader_u16_at(&feature, 4u + 2u * ((size_t)lookups - 1u),
                 &count) != GFNT_OK)) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, table, 0, GFNT_GLYPH_NONE,
        "a substitute feature table's lookup indices run past the table");
  }
  *out_feature_index = feature_index;
  *out_lookup_count = lookups;
  return GFNT_OK;
}

GFNT_Result gfnt_face_feature_substitution_lookup(const GFNT_Face * face,
    GFNT_Tag table, size_t record, size_t index, size_t position,
    uint16_t * out_lookup, GFNT_Error * error) {
  GFNT_FeatureVariations found;
  GFNT_Reader substitution;
  GFNT_Reader feature;
  uint16_t count = 0;
  uint16_t feature_index = 0;
  uint16_t lookups = 0;
  GFNT_Result result;

  if (!out_lookup) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_featurevar_open(face, table, &found, error);
  if (result == GFNT_OK) {
    result = gfnt_featurevar_substitution(&found, table, record, &substitution,
        &count, error);
  }
  if (result == GFNT_OK) {
    result = gfnt_featurevar_feature(&substitution, table, count, index,
        &feature_index, &feature, error);
  }
  if (result != GFNT_OK) {
    return result;
  }
  feature.error = error;
  if (gfnt_reader_u16_at(&feature, 2, &lookups) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, table, 0, GFNT_GLYPH_NONE,
        "a substitute feature table cut short");
  }
  if (position >= lookups) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, table, 0, GFNT_GLYPH_NONE,
        "this lookup position is past the substitute's count");
  }
  if (gfnt_reader_u16_at(&feature, 4u + 2u * position, out_lookup) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, table, 0, GFNT_GLYPH_NONE,
        "a substitute feature table's lookup indices run past the table");
  }
  return GFNT_OK;
}

/** One condition, printed as an expression; false-and-an-error on a bad table. */
static GFNT_Result gfnt_condition_print(const GFNT_Reader * parent,
    size_t offset, GFNT_Tag tag, unsigned depth, FILE * out, GFNT_Error * error) {
  GFNT_Reader condition;
  uint16_t format = 0;
  GFNT_Result result;

  if (depth > GFNT_FEATUREVAR_MAX_DEPTH) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, tag, offset, GFNT_GLYPH_NONE,
        "conditions nested deeper than this library follows");
  }
  result = gfnt_reader_sub(parent, offset, GFNT_READER_REST, &condition);
  if (result != GFNT_OK) {
    return result;
  }
  condition.error = error;
  if (gfnt_reader_u16_at(&condition, 0, &format) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, offset, GFNT_GLYPH_NONE,
        "a condition table cut short");
  }
  if (format == 1) {
    uint16_t axis = 0;
    int16_t low = 0;
    int16_t high = 0;

    if (gfnt_reader_u16_at(&condition, 2, &axis) != GFNT_OK
        || gfnt_reader_s16_at(&condition, 4, &low) != GFNT_OK
        || gfnt_reader_s16_at(&condition, 6, &high) != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, offset,
          GFNT_GLYPH_NONE, "a format 1 condition cut short");
    }
    return fprintf(out, "range %u %d %d", (unsigned)axis, (int)low, (int)high)
            < 0 ? GFNT_ERR_IO : GFNT_OK;
  }
  if (format == 3 || format == 4 || format == 5) {
    uint8_t count = 1;
    size_t first = 2;

    if (format != 5) {
      if (gfnt_reader_u8_at(&condition, 2, &count) != GFNT_OK) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, offset,
            GFNT_GLYPH_NONE, "an and/or condition cut short");
      }
      first = 3;
    }
    if (fprintf(out, "%s(", format == 3 ? "and" : format == 4 ? "or" : "not")
        < 0) {
      return GFNT_ERR_IO;
    }
    for (size_t i = 0; i < count; ++i) {
      uint8_t bytes[3];

      for (size_t k = 0; k < 3; ++k) {
        if (gfnt_reader_u8_at(&condition, first + 3u * i + k, &bytes[k])
            != GFNT_OK) {
          return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, offset,
              GFNT_GLYPH_NONE, "a condition's offsets run past it");
        }
      }
      if (i > 0 && fputc(',', out) == EOF) {
        return GFNT_ERR_IO;
      }
      result = gfnt_condition_print(&condition,
          ((size_t)bytes[0] << 16) | ((size_t)bytes[1] << 8) | bytes[2], tag,
          depth + 1, out, error);
      if (result != GFNT_OK) {
        return result;
      }
    }
    return fputc(')', out) == EOF ? GFNT_ERR_IO : GFNT_OK;
  }
  return fprintf(out, "format%u", (unsigned)format) < 0 ? GFNT_ERR_IO : GFNT_OK;
}

GFNT_Result gfnt_face_feature_variations_dump(const GFNT_Face * face,
    GFNT_Tag table, FILE * out) {
  GFNT_FeatureVariations found;
  GFNT_Result result;
  char name[5];

  if (!out) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_featurevar_open(face, table, &found, NULL);
  if (result != GFNT_OK) {
    return result;
  }
  gfnt_tag_string(table, name);
  if (found.count == 0) {
    return GFNT_OK;
  }
  if (fprintf(out, "%s FeatureVariations: %u records\n", name,
          (unsigned)found.count) < 0) {
    return GFNT_ERR_IO;
  }
  for (size_t r = 0; r < found.count; ++r) {
    GFNT_Reader set;
    GFNT_Reader substitution;
    uint16_t conditions = 0;
    uint16_t substitutions = 0;

    result = gfnt_featurevar_conditions(&found, table, r, &set, &conditions,
        NULL);
    if (result == GFNT_OK) {
      result = gfnt_featurevar_substitution(&found, table, r, &substitution,
          &substitutions, NULL);
    }
    if (result != GFNT_OK) {
      return result;
    }
    if (fprintf(out, "%s record %zu: %u conditions, %u substitutions\n", name,
            r, (unsigned)conditions, (unsigned)substitutions) < 0) {
      return GFNT_ERR_IO;
    }
    for (size_t c = 0; c < conditions; ++c) {
      uint32_t offset = 0;

      result = gfnt_featurevar_condition_offset(&set, table, c, &offset, NULL);
      if (result != GFNT_OK
          || fprintf(out, "%s record %zu condition %zu: ", name, r, c) < 0) {
        return result != GFNT_OK ? result : GFNT_ERR_IO;
      }
      result = gfnt_condition_print(&set, offset, table, 0, out, NULL);
      if (result != GFNT_OK || fputc('\n', out) == EOF) {
        return result != GFNT_OK ? result : GFNT_ERR_IO;
      }
    }
    for (size_t s = 0; s < substitutions; ++s) {
      uint16_t feature = 0;
      size_t lookups = 0;

      result = gfnt_face_feature_substitution_at(face, table, r, s, &feature,
          &lookups, NULL);
      if (result != GFNT_OK
          || fprintf(out, "%s record %zu substitution %zu: feature %u lookups",
                 name, r, s, (unsigned)feature) < 0) {
        return result != GFNT_OK ? result : GFNT_ERR_IO;
      }
      for (size_t k = 0; k < lookups; ++k) {
        uint16_t lookup = 0;

        result = gfnt_face_feature_substitution_lookup(face, table, r, s, k,
            &lookup, NULL);
        if (result != GFNT_OK || fprintf(out, " %u", (unsigned)lookup) < 0) {
          return result != GFNT_OK ? result : GFNT_ERR_IO;
        }
      }
      if (fputc('\n', out) == EOF) {
        return GFNT_ERR_IO;
      }
    }
  }
  return GFNT_OK;
}
