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
 * The `feat` reader. See `feat.h` in the public headers.
 *
 * Nothing is parsed ahead of a call: a lookup reads the header and then the one record it was
 * asked for, from the blob.
 */

#include <ghoti.io/font/feat.h>
#include <ghoti.io/font/macros.h>
#include <stdio.h>
#include "../reader/reader.h"
#include "../sfnt/sfnt.h"
#include "../tables/tables.h"

#define GFNT_TAG_feat GFNT_TAG('f', 'e', 'a', 't')

// The header (version, count, two reserved fields), a feature record, and a setting record.
#define GFNT_FEAT_HEADER 12u
#define GFNT_FEAT_RECORD 12u
#define GFNT_FEAT_SETTING 4u

#define GFNT_FEAT_EXCLUSIVE 0x8000u
#define GFNT_FEAT_DEFAULT 0x4000u

bool gfnt_face_has_feat(const GFNT_Face * face) {
  return face && gfnt_face_has_table(face, GFNT_TAG_feat);
}

static GFNT_Result gfnt_feat_open(const GFNT_Face * face, GFNT_Reader * table, uint16_t * count,
    GFNT_Error * error) {
  uint32_t version = 0;
  GFNT_Result result;

  gfnt_error_clear(error);
  if (!face) {
    return GFNT_ERR_INVALID;
  }
  if (!gfnt_face_has_table(face, GFNT_TAG_feat)) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_feat, 0, GFNT_GLYPH_NONE,
        "the face has no feat");
  }
  result = gfnt_face_table_reader(face, GFNT_TAG_feat, table, error);
  if (result != GFNT_OK) {
    return result;
  }
  table->error = error;
  if (table->length < GFNT_FEAT_HEADER || gfnt_reader_u32_at(table, 0, &version) != GFNT_OK
      || gfnt_reader_u16_at(table, 4, count) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_feat, 0, GFNT_GLYPH_NONE,
        "a feat shorter than its own header");
  }
  if ((version >> 16) != 1u) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_feat, 0, GFNT_GLYPH_NONE,
        "a feat version other than 1.x");
  }
  if ((size_t)*count * GFNT_FEAT_RECORD > table->length - GFNT_FEAT_HEADER) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_feat, 4, GFNT_GLYPH_NONE,
        "the feat feature records run past the table");
  }
  return GFNT_OK;
}

GFNT_Result gfnt_face_feat_count(const GFNT_Face * face, size_t * out_count, GFNT_Error * error) {
  GFNT_Reader table;
  uint16_t count = 0;
  GFNT_Result result;

  if (!out_count) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_feat_open(face, &table, &count, error);
  if (result == GFNT_OK) {
    *out_count = count;
  }
  return result;
}

/** One feature record and where its settings are, checked to be inside the table. */
static GFNT_Result gfnt_feat_record(const GFNT_Reader * table, uint16_t count, size_t index,
    GFNT_FeatFeature * out, size_t * settings_at, GFNT_Error * error) {
  size_t at = GFNT_FEAT_HEADER + index * GFNT_FEAT_RECORD;
  uint16_t nsettings = 0;
  uint32_t offset = 0;
  uint16_t flags = 0;
  uint16_t name = 0;

  if (index >= count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_feat, 0, GFNT_GLYPH_NONE,
        "this feat feature index is past the count");
  }
  if (gfnt_reader_u16_at(table, at, &out->type) != GFNT_OK
      || gfnt_reader_u16_at(table, at + 2, &nsettings) != GFNT_OK
      || gfnt_reader_u32_at(table, at + 4, &offset) != GFNT_OK
      || gfnt_reader_u16_at(table, at + 8, &flags) != GFNT_OK
      || gfnt_reader_u16_at(table, at + 10, &name) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_feat, at, GFNT_GLYPH_NONE,
        "a feat feature record runs past the table");
  }
  if ((size_t)offset > table->length
      || (size_t)nsettings * GFNT_FEAT_SETTING > table->length - (size_t)offset) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_feat, at, GFNT_GLYPH_NONE,
        "a feat feature's settings run past the table");
  }
  out->name_id = name;
  out->flags = flags;
  out->selector_count = nsettings;
  out->exclusive = (flags & GFNT_FEAT_EXCLUSIVE) != 0;
  out->has_default = (flags & GFNT_FEAT_DEFAULT) != 0;
  out->default_index = out->has_default ? (uint8_t)(flags & 0xFFu) : 0;
  *settings_at = offset;
  return GFNT_OK;
}

GFNT_Result gfnt_face_feat_at(const GFNT_Face * face, size_t index, GFNT_FeatFeature * out,
    GFNT_Error * error) {
  GFNT_Reader table;
  uint16_t count = 0;
  size_t settings = 0;
  GFNT_Result result;

  if (!out) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_feat_open(face, &table, &count, error);
  if (result != GFNT_OK) {
    return result;
  }
  return gfnt_feat_record(&table, count, index, out, &settings, error);
}

GFNT_Result gfnt_face_feat_selector_at(const GFNT_Face * face, size_t feature_index,
    size_t selector_index, GFNT_FeatSelector * out, GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_FeatFeature feature;
  uint16_t count = 0;
  size_t settings = 0;
  size_t at;
  uint16_t name = 0;
  GFNT_Result result;

  if (!out) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_feat_open(face, &table, &count, error);
  if (result == GFNT_OK) {
    result = gfnt_feat_record(&table, count, feature_index, &feature, &settings, error);
  }
  if (result != GFNT_OK) {
    return result;
  }
  if (selector_index >= feature.selector_count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_feat, 0, GFNT_GLYPH_NONE,
        "this feat setting index is past the feature's count");
  }
  at = settings + selector_index * GFNT_FEAT_SETTING;
  if (gfnt_reader_u16_at(&table, at, &out->selector) != GFNT_OK
      || gfnt_reader_u16_at(&table, at + 2, &name) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_feat, at, GFNT_GLYPH_NONE,
        "a feat setting runs past the table");
  }
  out->name_id = name;
  return GFNT_OK;
}

GFNT_Result gfnt_face_feat_dump(const GFNT_Face * face, FILE * out) {
  GFNT_Reader table;
  uint16_t count = 0;
  GFNT_Result result;

  if (!out) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_feat_open(face, &table, &count, NULL);
  if (result != GFNT_OK) {
    return result;
  }
  if (fprintf(out, "feat: %u features\n", (unsigned)count) < 0) {
    return GFNT_ERR_IO;
  }
  for (size_t i = 0; i < count; ++i) {
    GFNT_FeatFeature feature;
    size_t settings = 0;

    result = gfnt_feat_record(&table, count, i, &feature, &settings, NULL);
    if (result != GFNT_OK) {
      return result;
    }
    if (fprintf(out, "feat feature %zu: type %u flags 0x%04X name %u settings %u\n", i,
            (unsigned)feature.type, (unsigned)feature.flags, (unsigned)feature.name_id,
            (unsigned)feature.selector_count) < 0) {
      return GFNT_ERR_IO;
    }
    for (size_t k = 0; k < feature.selector_count; ++k) {
      GFNT_FeatSelector sel;

      result = gfnt_face_feat_selector_at(face, i, k, &sel, NULL);
      if (result != GFNT_OK) {
        return result;
      }
      if (fprintf(out, "feat setting %zu.%zu: selector %u name %u\n", i, k, (unsigned)sel.selector,
              (unsigned)sel.name_id) < 0) {
        return GFNT_ERR_IO;
      }
    }
  }
  return GFNT_OK;
}
