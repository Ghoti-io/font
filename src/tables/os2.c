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
 * The `OS/2` table, versions 0 to 5.
 *
 * documentation/design.md section 7.2, and section 17.9 for `fsType`: the
 * embedding permissions are **reported and never enforced**, because a library
 * that refuses to render a font over an embedding bit breaks every legitimate
 * viewer, and one that embeds a font into a document without saying so breaks
 * the law for its caller.
 *
 * A table shorter than the version it declares is ::GFNT_ERR_CORRUPT rather
 * than a partial parse. The alternative - report the fields that are there and
 * quietly demote the version - would answer a question about `sCapHeight` with
 * a zero that looks like a font's own value, and a caller cannot tell those
 * apart. The diagnostic says which version was claimed and how short the table
 * is, which is what a font with a truncated `OS/2` needs to say.
 *
 * Reference: OpenType Specification 1.9, "OS/2 - OS/2 and Windows Metrics
 * Table".
 */

#include <ghoti.io/font/macros.h>
#include "tables.h"

/** Bytes each version's fields occupy, indexed by version. */
static const size_t gfnt_os2_lengths[] = {78, 86, 96, 96, 96, 100};

GFNT_Result gfnt_os2_parse(const GFNT_Face * face, void * out,
    GFNT_Error * error) {
  static const GFNT_Tag tag = GFNT_TAG('O', 'S', '/', '2');
  GFNT_Os2 * os2 = out;
  GFNT_Reader reader;
  const uint8_t * panose = NULL;
  const uint8_t * vendor = NULL;
  GFNT_Result result;

  result = gfnt_face_table_reader(face, tag, &reader, error);
  if (result != GFNT_OK) {
    return result;
  }

  *os2 = (GFNT_Os2) {0};
  if (gfnt_read_u16(&reader, &os2->version) != GFNT_OK) {
    return GFNT_ERR_CORRUPT;
  }
  if (os2->version >= GFNT_ARRAY_SIZE(gfnt_os2_lengths)) {
    // A version nobody has written yet. Not corrupt: the file is well-formed
    // and this library is the one that is behind.
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, tag, 0, GFNT_GLYPH_NONE,
        "an OS/2 version this library does not read");
  }
  if (reader.length < gfnt_os2_lengths[os2->version]) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, tag, reader.length,
        GFNT_GLYPH_NONE,
        "the OS/2 table is shorter than the version it declares");
  }

  if (gfnt_read_s16(&reader, &os2->x_avg_char_width) != GFNT_OK
      || gfnt_read_u16(&reader, &os2->weight_class) != GFNT_OK
      || gfnt_read_u16(&reader, &os2->width_class) != GFNT_OK
      || gfnt_read_u16(&reader, &os2->fs_type) != GFNT_OK
      || gfnt_read_s16(&reader, &os2->subscript_x_size) != GFNT_OK
      || gfnt_read_s16(&reader, &os2->subscript_y_size) != GFNT_OK
      || gfnt_read_s16(&reader, &os2->subscript_x_offset) != GFNT_OK
      || gfnt_read_s16(&reader, &os2->subscript_y_offset) != GFNT_OK
      || gfnt_read_s16(&reader, &os2->superscript_x_size) != GFNT_OK
      || gfnt_read_s16(&reader, &os2->superscript_y_size) != GFNT_OK
      || gfnt_read_s16(&reader, &os2->superscript_x_offset) != GFNT_OK
      || gfnt_read_s16(&reader, &os2->superscript_y_offset) != GFNT_OK
      || gfnt_read_s16(&reader, &os2->strikeout_size) != GFNT_OK
      || gfnt_read_s16(&reader, &os2->strikeout_position) != GFNT_OK
      || gfnt_read_s16(&reader, &os2->family_class) != GFNT_OK
      || gfnt_read_bytes(&reader, 10, &panose) != GFNT_OK) {
    return GFNT_ERR_CORRUPT;
  }
  for (size_t i = 0; i < 10; ++i) {
    os2->panose[i] = panose[i];
  }

  for (size_t i = 0; i < 4; ++i) {
    if (gfnt_read_u32(&reader, &os2->unicode_range[i]) != GFNT_OK) {
      return GFNT_ERR_CORRUPT;
    }
  }
  if (gfnt_read_bytes(&reader, 4, &vendor) != GFNT_OK) {
    return GFNT_ERR_CORRUPT;
  }
  for (size_t i = 0; i < 4; ++i) {
    // Kept as the file gives it: this is an identifier a caller may want to
    // compare, so sanitising belongs in whatever prints it.
    os2->vendor_id[i] = (char)vendor[i];
  }
  os2->vendor_id[4] = '\0';

  if (gfnt_read_u16(&reader, &os2->fs_selection) != GFNT_OK
      || gfnt_read_u16(&reader, &os2->first_char_index) != GFNT_OK
      || gfnt_read_u16(&reader, &os2->last_char_index) != GFNT_OK
      || gfnt_read_s16(&reader, &os2->typo_ascender) != GFNT_OK
      || gfnt_read_s16(&reader, &os2->typo_descender) != GFNT_OK
      || gfnt_read_s16(&reader, &os2->typo_line_gap) != GFNT_OK
      || gfnt_read_u16(&reader, &os2->win_ascent) != GFNT_OK
      || gfnt_read_u16(&reader, &os2->win_descent) != GFNT_OK) {
    return GFNT_ERR_CORRUPT;
  }

  if (os2->version >= 1) {
    if (gfnt_read_u32(&reader, &os2->code_page_range[0]) != GFNT_OK
        || gfnt_read_u32(&reader, &os2->code_page_range[1]) != GFNT_OK) {
      return GFNT_ERR_CORRUPT;
    }
  }
  if (os2->version >= 2) {
    if (gfnt_read_s16(&reader, &os2->x_height) != GFNT_OK
        || gfnt_read_s16(&reader, &os2->cap_height) != GFNT_OK
        || gfnt_read_u16(&reader, &os2->default_char) != GFNT_OK
        || gfnt_read_u16(&reader, &os2->break_char) != GFNT_OK
        || gfnt_read_u16(&reader, &os2->max_context) != GFNT_OK) {
      return GFNT_ERR_CORRUPT;
    }
  }
  if (os2->version >= 5) {
    if (gfnt_read_u16(&reader, &os2->lower_optical_size) != GFNT_OK
        || gfnt_read_u16(&reader, &os2->upper_optical_size) != GFNT_OK) {
      return GFNT_ERR_CORRUPT;
    }
  }
  return GFNT_OK;
}

GFNT_Result gfnt_os2_dump(const GFNT_Os2 * os2, FILE * out) {
  char vendor[5];

  if (!os2 || !out) {
    return GFNT_ERR_INVALID;
  }
  for (size_t i = 0; i < 4; ++i) {
    unsigned char byte = (unsigned char)os2->vendor_id[i];
    vendor[i] = (byte >= 0x20u && byte < 0x7Fu) ? (char)byte : '.';
  }
  vendor[4] = '\0';

  if (fprintf(out,
          "OS/2: version %u, weightClass %u, widthClass %u, fsType 0x%04X, "
          "fsSelection 0x%04X\n",
          os2->version, os2->weight_class, os2->width_class, os2->fs_type,
          os2->fs_selection)
      < 0) {
    return GFNT_ERR_IO;
  }
  if (fprintf(out,
          "OS/2: typo %d/%d/%d, win %u/%u, xAvgCharWidth %d, vendor '%s'\n",
          os2->typo_ascender, os2->typo_descender, os2->typo_line_gap,
          os2->win_ascent, os2->win_descent, os2->x_avg_char_width, vendor)
      < 0) {
    return GFNT_ERR_IO;
  }
  if (fprintf(out,
          "OS/2: subscript %d/%d/%d/%d, superscript %d/%d/%d/%d, "
          "strikeout %d/%d\n",
          os2->subscript_x_size, os2->subscript_y_size,
          os2->subscript_x_offset, os2->subscript_y_offset,
          os2->superscript_x_size, os2->superscript_y_size,
          os2->superscript_x_offset, os2->superscript_y_offset,
          os2->strikeout_size, os2->strikeout_position)
      < 0) {
    return GFNT_ERR_IO;
  }
  if (fprintf(out,
          "OS/2: familyClass %d, panose %u %u %u %u %u %u %u %u %u %u\n",
          os2->family_class, os2->panose[0], os2->panose[1], os2->panose[2],
          os2->panose[3], os2->panose[4], os2->panose[5], os2->panose[6],
          os2->panose[7], os2->panose[8], os2->panose[9])
      < 0) {
    return GFNT_ERR_IO;
  }
  if (fprintf(out,
          "OS/2: unicodeRange 0x%08X 0x%08X 0x%08X 0x%08X, "
          "codePageRange 0x%08X 0x%08X\n",
          (unsigned)os2->unicode_range[0], (unsigned)os2->unicode_range[1],
          (unsigned)os2->unicode_range[2], (unsigned)os2->unicode_range[3],
          (unsigned)os2->code_page_range[0], (unsigned)os2->code_page_range[1])
      < 0) {
    return GFNT_ERR_IO;
  }
  if (fprintf(out,
          "OS/2: firstCharIndex %u, lastCharIndex %u, xHeight %d, "
          "capHeight %d\n",
          os2->first_char_index, os2->last_char_index, os2->x_height,
          os2->cap_height)
      < 0) {
    return GFNT_ERR_IO;
  }
  if (fprintf(out,
          "OS/2: defaultChar %u, breakChar %u, maxContext %u, "
          "opticalSize %u..%u\n",
          os2->default_char, os2->break_char, os2->max_context,
          os2->lower_optical_size, os2->upper_optical_size)
      < 0) {
    return GFNT_ERR_IO;
  }
  return GFNT_OK;
}
