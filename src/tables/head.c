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
 * The `head` table: the em square, the bounding box, and the two format
 * fields a glyph reader cannot do without.
 *
 * documentation/design.md section 7.2.
 *
 * Reference: OpenType Specification 1.9, "head - Font Header Table"; the Apple
 * TrueType Reference Manual, "The 'head' table".
 */

#include <ghoti.io/font/macros.h>
#include "tables.h"

/** What every `head` table's magicNumber is, and has been since 1991. */
#define GFNT_HEAD_MAGIC 0x5F0F3CF5u

GFNT_Result gfnt_head_parse(const GFNT_Face * face, void * out,
    GFNT_Error * error) {
  static const GFNT_Tag tag = GFNT_TAG('h', 'e', 'a', 'd');
  GFNT_Head * head = out;
  GFNT_Reader reader;
  uint32_t magic = 0;
  GFNT_Result result;

  result = gfnt_face_table_reader(face, tag, &reader, error);
  if (result != GFNT_OK) {
    return result;
  }

  *head = (GFNT_Head) {0};
  if (gfnt_read_u16(&reader, &head->major_version) != GFNT_OK
      || gfnt_read_u16(&reader, &head->minor_version) != GFNT_OK
      || gfnt_read_fixed(&reader, &head->font_revision) != GFNT_OK
      || gfnt_read_u32(&reader, &head->checksum_adjustment) != GFNT_OK
      || gfnt_read_u32(&reader, &magic) != GFNT_OK
      || gfnt_read_u16(&reader, &head->flags) != GFNT_OK
      || gfnt_read_u16(&reader, &head->units_per_em) != GFNT_OK
      || gfnt_read_longdatetime(&reader, &head->created) != GFNT_OK
      || gfnt_read_longdatetime(&reader, &head->modified) != GFNT_OK
      || gfnt_read_s16(&reader, &head->x_min) != GFNT_OK
      || gfnt_read_s16(&reader, &head->y_min) != GFNT_OK
      || gfnt_read_s16(&reader, &head->x_max) != GFNT_OK
      || gfnt_read_s16(&reader, &head->y_max) != GFNT_OK
      || gfnt_read_u16(&reader, &head->mac_style) != GFNT_OK
      || gfnt_read_u16(&reader, &head->lowest_rec_ppem) != GFNT_OK
      || gfnt_read_s16(&reader, &head->font_direction_hint) != GFNT_OK
      || gfnt_read_s16(&reader, &head->index_to_loc_format) != GFNT_OK
      || gfnt_read_s16(&reader, &head->glyph_data_format) != GFNT_OK) {
    // The reader has already recorded which byte it ran out at.
    return GFNT_ERR_CORRUPT;
  }

  if (magic != GFNT_HEAD_MAGIC) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, 12, GFNT_GLYPH_NONE,
        "head magicNumber is not 0x5F0F3CF5");
  }
  if (head->units_per_em == 0) {
    // The specification's range is 16 to 16384 and this library does not
    // enforce it - fonts outside it exist and render elsewhere - but zero is
    // different in kind: every scale derived from it is a division by zero, so
    // there is no behaviour to be permissive about.
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, 18, GFNT_GLYPH_NONE,
        "head unitsPerEm is zero, so nothing can be scaled");
  }
  if (head->index_to_loc_format != 0 && head->index_to_loc_format != 1) {
    // This field selects a format rather than describing one, so a third value
    // names no format at all and `loca` cannot be read.
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, 50, GFNT_GLYPH_NONE,
        "head indexToLocFormat is neither short nor long");
  }
  return GFNT_OK;
}

GFNT_Result gfnt_head_dump(const GFNT_Head * head, FILE * out) {
  if (!head || !out) {
    return GFNT_ERR_INVALID;
  }
  if (fprintf(out,
          "head: version %u.%u, revision 0x%08X, unitsPerEm %u, flags 0x%04X\n",
          head->major_version, head->minor_version,
          (unsigned)head->font_revision, head->units_per_em, head->flags)
      < 0) {
    return GFNT_ERR_IO;
  }
  if (fprintf(out, "head: bbox %d %d %d %d, macStyle 0x%04X\n", head->x_min,
          head->y_min, head->x_max, head->y_max, head->mac_style)
      < 0) {
    return GFNT_ERR_IO;
  }
  if (fprintf(out,
          "head: created %lld, modified %lld, lowestRecPPEM %u\n",
          (long long)head->created, (long long)head->modified,
          head->lowest_rec_ppem)
      < 0) {
    return GFNT_ERR_IO;
  }
  if (fprintf(out,
          "head: indexToLocFormat %d, glyphDataFormat %d, "
          "fontDirectionHint %d, checkSumAdjustment 0x%08X\n",
          head->index_to_loc_format, head->glyph_data_format,
          head->font_direction_hint, (unsigned)head->checksum_adjustment)
      < 0) {
    return GFNT_ERR_IO;
  }
  return GFNT_OK;
}
