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
 * The `post` table's header fields.
 *
 * documentation/design.md section 7.2. **Glyph names are not here yet.**
 * Format 2.0 spells the first 258 of them as indices into the standard
 * Macintosh glyph order, and that list is a 258-entry vector: section 14's
 * rule is that a vector comes from an oracle and is never written from memory,
 * and fontTools is where this one lives. The header fields below need no such
 * table, so they arrive now and the names arrive with the fixture generator.
 *
 * Reference: OpenType Specification 1.9, "post - PostScript Table".
 */

#include <ghoti.io/font/macros.h>
#include "tables.h"

GFNT_Result gfnt_post_parse(const GFNT_Face * face, void * out,
    GFNT_Error * error) {
  static const GFNT_Tag tag = GFNT_TAG('p', 'o', 's', 't');
  GFNT_Post * post = out;
  GFNT_Reader reader;
  GFNT_Result result;

  result = gfnt_face_table_reader(face, tag, &reader, error);
  if (result != GFNT_OK) {
    return result;
  }

  *post = (GFNT_Post) {0};
  if (gfnt_read_fixed(&reader, &post->version) != GFNT_OK
      || gfnt_read_fixed(&reader, &post->italic_angle) != GFNT_OK
      || gfnt_read_s16(&reader, &post->underline_position) != GFNT_OK
      || gfnt_read_s16(&reader, &post->underline_thickness) != GFNT_OK
      || gfnt_read_u32(&reader, &post->is_fixed_pitch) != GFNT_OK
      || gfnt_read_u32(&reader, &post->min_mem_type42) != GFNT_OK
      || gfnt_read_u32(&reader, &post->max_mem_type42) != GFNT_OK
      || gfnt_read_u32(&reader, &post->min_mem_type1) != GFNT_OK
      || gfnt_read_u32(&reader, &post->max_mem_type1) != GFNT_OK) {
    return GFNT_ERR_CORRUPT;
  }

  // Every version's header is these same 32 bytes, and what follows them
  // differs: 1.0 nothing, 2.0 the name index and the names, 2.5 a deprecated
  // offset array, 3.0 nothing. The version is reported rather than checked,
  // because the header is readable whatever follows it and a caller asking for
  // italicAngle should not be refused by a version it never mentioned.
  return GFNT_OK;
}

GFNT_Result gfnt_post_dump(const GFNT_Post * post, FILE * out) {
  if (!post || !out) {
    return GFNT_ERR_INVALID;
  }
  if (fprintf(out, "post: version 0x%08X, italicAngle 0x%08X, isFixedPitch %u\n",
          (unsigned)post->version, (unsigned)post->italic_angle,
          (unsigned)post->is_fixed_pitch)
      < 0) {
    return GFNT_ERR_IO;
  }
  if (fprintf(out, "post: underlinePosition %d, underlineThickness %d\n",
          post->underline_position, post->underline_thickness)
      < 0) {
    return GFNT_ERR_IO;
  }
  if (fprintf(out, "post: minMemType42 %u, maxMemType42 %u, minMemType1 %u, "
                   "maxMemType1 %u\n",
          (unsigned)post->min_mem_type42, (unsigned)post->max_mem_type42,
          (unsigned)post->min_mem_type1, (unsigned)post->max_mem_type1)
      < 0) {
    return GFNT_ERR_IO;
  }
  return GFNT_OK;
}
