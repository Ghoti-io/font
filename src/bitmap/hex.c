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
 * GNU Unifont's `.hex`: a codepoint, a colon, and the glyph in hexadecimal.
 *
 * documentation/design.md sections 7.1 and 7.5. The simplest font format there
 * is, and the only one here with **no magic and no header at all** - which
 * decides two things about how it is read.
 *
 * First, the probe is the grammar: a file is `.hex` when its first content line
 * is a codepoint, a colon and a whole number of rows of hexadecimal and nothing
 * else. So it is tried last of the four, after three formats that identify
 * themselves in their first bytes.
 *
 * Second, **the file states no baseline**, no advance, no side bearing and no
 * name - only sixteen rows per glyph. This library reports the box it is given,
 * with the top row at the ascent line and an advance equal to the width, and
 * ::GFNT_BitmapFont::states_baseline is false to say that no baseline was read
 * from anywhere. Unifont's own BDFs put it two rows above the bottom; that is
 * Unifont's, not the format's, and a reader that hardcoded it would be
 * describing one font while claiming to describe a format.
 *
 * Reference: unifont-hex(5) and the GNU Unifont manual, "The .hex Format".
 */

#include <ghoti.io/cutil/safemath.h>
#include <ghoti.io/font/macros.h>
#include "../sfnt/sfnt.h"
#include "bitmap.h"

/** Rows in a `.hex` glyph. The format has exactly one height. */
#define GFNT_HEX_ROWS 16u
/** The widest glyph the format defines: 32 pixels, four bytes a row. */
#define GFNT_HEX_MAX_STRIDE 4u
/** Digits in the shortest legal codepoint field, and in the longest. */
#define GFNT_HEX_CODE_MIN 4u
#define GFNT_HEX_CODE_MAX 6u

/** The value of one hexadecimal digit, or 16 for anything else. */
static unsigned gfnt_hex_digit(uint8_t byte) {
  if (byte >= '0' && byte <= '9') {
    return (unsigned)(byte - '0');
  }
  if (byte >= 'a' && byte <= 'f') {
    return (unsigned)(byte - 'a') + 10u;
  }
  if (byte >= 'A' && byte <= 'F') {
    return (unsigned)(byte - 'A') + 10u;
  }
  return 16u;
}

/**
 * Split one line into its codepoint and its rows, and check the shape.
 *
 * @param line The line's bytes.
 * @param length Its length.
 * @param out_code Receives the codepoint.
 * @param out_digits Receives where the bitmap digits start.
 * @param out_count Receives how many there are.
 * @return Whether the line is a `.hex` record.
 */
static bool gfnt_hex_split(const uint8_t * line, size_t length,
    uint32_t * out_code, size_t * out_digits, size_t * out_count) {
  size_t at = 0;
  uint32_t code = 0;
  size_t count;

  while (at < length && gfnt_hex_digit(line[at]) < 16u) {
    code = (code << 4) | gfnt_hex_digit(line[at]);
    ++at;
    if (at > GFNT_HEX_CODE_MAX) {
      return false;
    }
  }
  if (at < GFNT_HEX_CODE_MIN || at >= length || line[at] != ':') {
    return false;
  }
  *out_code = code;
  *out_digits = at + 1;
  count = length - *out_digits;
  for (size_t i = 0; i < count; ++i) {
    if (gfnt_hex_digit(line[*out_digits + i]) >= 16u) {
      return false;
    }
  }
  // Sixteen rows of a whole number of bytes: 32 digits for an 8-pixel glyph, 64
  // for 16, and so on. A count that is not a multiple of 32 is not a glyph of
  // this format at any width.
  if (count == 0 || count % (2u * GFNT_HEX_ROWS) != 0
      || count / (2u * GFNT_HEX_ROWS) > GFNT_HEX_MAX_STRIDE) {
    return false;
  }
  *out_count = count;
  return true;
}

bool gfnt_hex_looks_like(const GFNT_Reader * blob) {
  GFNT_Reader reader = *blob;
  GFNT_Lines lines;
  const uint8_t * line = NULL;
  size_t length = 0;
  bool more = false;
  uint32_t code = 0;
  size_t digits = 0;
  size_t count = 0;

  if (gfnt_reader_seek(&reader, 0) != GFNT_OK
      || gfnt_lines_init(&lines, &reader, GFNT_HEX_CODE_MAX + 1
              + 2u * GFNT_HEX_ROWS * GFNT_HEX_MAX_STRIDE) != GFNT_OK) {
    return false;
  }
  // One line decides it, and the cap above is why that is enough: the line
  // reader refuses anything longer than the longest legal record, so a file of
  // prose is rejected by its first line rather than scanned.
  while (gfnt_lines_next(&lines, &line, &length, &more, NULL) == GFNT_OK
      && more) {
    if (length == 0) {
      continue;
    }
    return gfnt_hex_split(line, length, &code, &digits, &count);
  }
  return false;
}

GFNT_Result gfnt_hex_parse(const GFNT_Face * face, void * out_font,
    void * context,
    GFNT_Error * error) {
  // There is one of this table per face, so there is nothing to select; the
  // parameter is here because every memo parser shares one signature.
  (void)context;
  GFNT_BitmapFont * font = out_font;
  GFNT_BitmapBuild * build = NULL;
  GFNT_Reader reader;
  GFNT_Lines lines;
  GFNT_Strike * strike;
  const uint8_t * line = NULL;
  size_t length = 0;
  bool more = false;
  GFNT_Result result;

  result = gfnt_face_table_reader(face, GFNT_TAG_HEX, &reader, error);
  if (result != GFNT_OK) {
    return result;
  }
  // The glyph count is not stated anywhere - there is no header to state it in -
  // so the builder starts at zero and grows. That is the one place `.hex` costs
  // more than the other three, and it is why the limit is checked per glyph
  // below as well as once at the start.
  result = gfnt_bitmap_build_start(&build, face, GFNT_FLAVOUR_HEX, 0, error);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_lines_init(&lines, &reader, face->limits.max_line_length);
  if (result != GFNT_OK) {
    gfnt_bitmap_build_finish(build, NULL, false);
    return gfnt_error_set(error, result, GFNT_TAG_HEX, 0, GFNT_GLYPH_NONE,
        "starting to read the file's lines");
  }

  while ((result = gfnt_lines_next(&lines, &line, &length, &more, error))
          == GFNT_OK
      && more) {
    uint8_t rows[GFNT_HEX_ROWS * GFNT_HEX_MAX_STRIDE];
    GFNT_BitmapRecord record;
    uint32_t code = 0;
    size_t digits = 0;
    size_t count = 0;
    size_t stride;
    uint32_t glyph;

    if (length == 0) {
      continue;
    }
    if (!gfnt_hex_split(line, length, &code, &digits, &count)) {
      result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_HEX,
          lines.offset, GFNT_GLYPH_NONE,
          "a line that is not a codepoint, a colon and sixteen rows of "
          "hexadecimal");
      break;
    }
    stride = count / (2u * GFNT_HEX_ROWS);
    for (size_t i = 0; i < count / 2u; ++i) {
      rows[i] = (uint8_t)((gfnt_hex_digit(line[digits + 2u * i]) << 4)
          | gfnt_hex_digit(line[digits + 2u * i + 1u]));
    }

    glyph = (uint32_t)gfnt_bitmap_build_count(build);
    if (glyph >= face->limits.max_glyphs) {
      result = gfnt_error_set(error, GFNT_ERR_LIMIT, GFNT_TAG_HEX, lines.offset,
          glyph, "more glyphs than GFNT_Limits::max_glyphs");
      break;
    }
    record = (GFNT_BitmapRecord) {
      .width = (uint32_t)(stride * 8u),
      .height = GFNT_HEX_ROWS,
      .bearing_x = 0,
      // No baseline is stated, so the box is reported where it is: its top row
      // at the ascent line. states_baseline says that this is the format's
      // silence and not a measurement.
      .bearing_y = (int32_t)GFNT_HEX_ROWS,
      .advance = (int32_t)(stride * 8u),
    };
    result = gfnt_bitmap_build_glyph(build, rows, stride,
        GFNT_ORDER_MSB_FIRST, GFNT_ORDER_MSB_FIRST, 1, &record, NULL, 0, error);
    if (result != GFNT_OK) {
      break;
    }
    result = gfnt_bitmap_build_map(build, code, glyph, error);
    if (result != GFNT_OK) {
      break;
    }
  }
  if (result != GFNT_OK) {
    gfnt_bitmap_build_finish(build, NULL, false);
    return result;
  }
  if (gfnt_bitmap_build_count(build) == 0) {
    gfnt_bitmap_build_finish(build, NULL, false);
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_HEX, 0,
        GFNT_GLYPH_NONE, "a .hex file with no glyphs in it");
  }

  strike = gfnt_bitmap_build_strike(build);
  strike->index = 0;
  strike->ppem_x = GFNT_HEX_ROWS;
  strike->ppem_y = GFNT_HEX_ROWS;
  strike->ascent = (int32_t)GFNT_HEX_ROWS;
  strike->descent = 0;
  gfnt_bitmap_build_font(build)->states_encoding = true;
  gfnt_bitmap_build_font(build)->states_baseline = false;
  gfnt_bitmap_build_finish(build, font, true);
  return GFNT_OK;
}
