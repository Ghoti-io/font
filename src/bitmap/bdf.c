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
 * BDF: the Bitmap Distribution Format, which is a font written as text.
 *
 * documentation/design.md sections 7.1 and 7.5. A keyword per line, a property
 * list, and then a `STARTCHAR`/`ENDCHAR` block per glyph whose bitmap is rows of
 * hexadecimal. It is the only one of the four formats that states **per-glyph
 * boxes and per-glyph advances**, which is why it is also the fixture that
 * exercises negative bearings and an advance that is not the width.
 *
 * Three things a reader has to get right, each of which is a real file somewhere:
 *
 *  - **`BBX y` is the bottom, not the top.** The box is `w h x y` with `y` the
 *    offset of its *lowest* row from the baseline, so the bearing this library
 *    reports - the top edge, y-up (section 5.5) - is `y + h`. Reading `y` as the
 *    top puts every descender above the line and looks like a font bug.
 *  - **`ENCODING -1` is a glyph with no character.** It is how a font ships a
 *    glyph reachable only by name; it must be a glyph in the order and absent
 *    from the map, and dropping it renumbers everything after it.
 *  - **A row may carry more digits than the glyph is wide.** Writers pad to the
 *    font's bounding box rather than the glyph's. The extra digits are ignored
 *    rather than refused, because refusing them would refuse fonts that draw
 *    correctly in every other reader.
 *
 * Reference: Adobe *Glyph Bitmap Distribution Format (BDF) Specification*,
 * version 2.2 (Adobe Technical Note #5005).
 */

#include <ghoti.io/cutil/safemath.h>
#include <ghoti.io/font/macros.h>
#include <string.h>
#include "../sfnt/sfnt.h"
#include "bitmap.h"

/** How many bytes of a line the probe will read looking for `STARTFONT`. */
#define GFNT_BDF_PROBE_BYTES 256u

/** One token of a line: where it is and how long. */
typedef struct GFNT_BdfToken {
  const char * text;
  size_t length;
} GFNT_BdfToken;

/** Whether a byte separates tokens. */
static bool gfnt_bdf_space(uint8_t byte) {
  return byte == ' ' || byte == '\t';
}

/**
 * The first token of a line - its keyword - and where the rest starts.
 *
 * @param line The line.
 * @param length Its length.
 * @param out_keyword Receives the keyword.
 * @param out_rest Receives the offset of the first byte after it.
 * @return Whether there was a token at all.
 */
static bool gfnt_bdf_keyword(const uint8_t * line, size_t length,
    GFNT_BdfToken * out_keyword, size_t * out_rest) {
  size_t at = 0;
  size_t start;

  while (at < length && gfnt_bdf_space(line[at])) {
    ++at;
  }
  start = at;
  while (at < length && !gfnt_bdf_space(line[at])) {
    ++at;
  }
  if (at == start) {
    return false;
  }
  out_keyword->text = (const char *)line + start;
  out_keyword->length = at - start;
  *out_rest = at;
  return true;
}

/** Whether a token is exactly this keyword. */
static bool gfnt_bdf_is(const GFNT_BdfToken * token, const char * keyword) {
  size_t length = strlen(keyword);

  return token->length == length && memcmp(token->text, keyword, length) == 0;
}

/**
 * Read a signed decimal integer at @p at, and leave @p at after it.
 *
 * Its own function rather than `strtol` because the line is not NUL-terminated -
 * it is a pointer into the blob - and because `strtol` reads the locale's idea of
 * a digit. A BDF number is ASCII in every file that exists.
 *
 * @return Whether one was there.
 */
static bool gfnt_bdf_number(const uint8_t * line, size_t length, size_t * at,
    int32_t * out_value) {
  bool negative = false;
  bool digits = false;
  int64_t value = 0;

  while (*at < length && gfnt_bdf_space(line[*at])) {
    *at += 1;
  }
  if (*at < length && (line[*at] == '-' || line[*at] == '+')) {
    negative = line[*at] == '-';
    *at += 1;
  }
  while (*at < length && line[*at] >= '0' && line[*at] <= '9') {
    value = value * 10 + (line[*at] - '0');
    digits = true;
    *at += 1;
    // A BDF number that does not fit a 32-bit integer describes no font, and
    // stopping here keeps the accumulation itself from overflowing.
    if (value > 0x7FFFFFFF) {
      return false;
    }
  }
  if (!digits) {
    return false;
  }
  *out_value = (int32_t)(negative ? -value : value);
  return true;
}

/** The value of one hexadecimal digit, or 16 for anything else. */
static unsigned gfnt_bdf_hex(uint8_t byte) {
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
 * A property's value as a string: the quoted text, or the bare token.
 *
 * BDF quotes a string value and doubles an embedded quote. The doubling is not
 * undone here - no property this library reads can contain one, and a name with
 * `""` in it is better reported as written than silently rewritten.
 */
static void gfnt_bdf_value(const uint8_t * line, size_t length, size_t at,
    GFNT_BdfToken * out_value) {
  while (at < length && gfnt_bdf_space(line[at])) {
    ++at;
  }
  if (at < length && line[at] == '"') {
    size_t end = at + 1;

    while (end < length && line[end] != '"') {
      ++end;
    }
    out_value->text = (const char *)line + at + 1;
    out_value->length = end - (at + 1);
    return;
  }
  out_value->text = (const char *)line + at;
  out_value->length = length - at;
  while (out_value->length > 0
      && gfnt_bdf_space((uint8_t)out_value->text[out_value->length - 1])) {
    out_value->length -= 1;
  }
}

bool gfnt_bdf_looks_like(const GFNT_Reader * blob) {
  static const char keyword[] = "STARTFONT";
  uint8_t byte = 0;
  size_t at = 0;

  // Leading blank lines are skipped, and nothing else is: a BDF begins with
  // STARTFONT, and a file whose first word is anything else is not one - not
  // even if it has a STARTFONT further down, which is what a scan of the whole
  // file would accept.
  while (gfnt_reader_u8_at(blob, at, &byte) == GFNT_OK
      && (byte == '\n' || byte == '\r' || gfnt_bdf_space(byte))) {
    ++at;
    if (at > GFNT_BDF_PROBE_BYTES) {
      return false;
    }
  }
  for (size_t i = 0; i < sizeof keyword - 1; ++i) {
    if (gfnt_reader_u8_at(blob, at + i, &byte) != GFNT_OK
        || byte != (uint8_t)keyword[i]) {
      return false;
    }
  }
  return true;
}

/** What the parse is in the middle of. */
typedef struct GFNT_BdfState {
  GFNT_BitmapBuild * build;
  GFNT_BitmapRecord record;   ///< The glyph being read.
  const char * name;          ///< Its name, pointing into the blob.
  size_t name_length;
  int32_t encoding;           ///< Its ENCODING, or -1 for none.
  bool have_bbx;
  bool have_advance;
  bool in_char;
  bool in_bitmap;
  uint32_t rows_read;
  uint8_t * rows;             ///< The glyph's rows being assembled.
  size_t row_capacity;
  size_t stride;

  int32_t font_ascent;
  int32_t font_descent;
  bool have_font_ascent;
  bool have_font_descent;
  int32_t pixel_size;
  bool have_pixel_size;
  int32_t box_height;
  int32_t box_offset_y;
  bool have_box;
  size_t chars_claimed;
  bool have_chars;
} GFNT_BdfState;

/**
 * Finish the glyph that is being read and add it to the builder.
 */
static GFNT_Result gfnt_bdf_end_char(GFNT_BdfState * state, size_t offset,
    GFNT_Error * error) {
  GFNT_Result result;
  uint32_t glyph = (uint32_t)gfnt_bitmap_build_count(state->build);

  if (!state->have_bbx) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_BDF, offset, glyph,
        "a character with no BBX, so no box to draw it in");
  }
  if (!state->have_advance) {
    // DWIDTH is required by the specification and a font without it states no
    // advance at all. Substituting the width would be inventing a metric, and a
    // caller laying out text would never find out.
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_BDF, offset, glyph,
        "a character with no DWIDTH, so no advance");
  }
  if (state->rows_read < state->record.height) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_BDF, offset, glyph,
        "a character whose BITMAP has fewer rows than its BBX");
  }
  result = gfnt_bitmap_build_glyph(state->build,
      state->record.height ? state->rows : NULL, state->stride,
      GFNT_ORDER_MSB_FIRST, GFNT_ORDER_MSB_FIRST, 1, &state->record,
      state->name, state->name_length, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (state->encoding >= 0) {
    result = gfnt_bitmap_build_map(state->build, (uint32_t)state->encoding,
        glyph, error);
    if (result != GFNT_OK) {
      return result;
    }
  }
  state->in_char = false;
  state->in_bitmap = false;
  return GFNT_OK;
}

/** One row of hexadecimal into the glyph's rows. */
static GFNT_Result gfnt_bdf_bitmap_row(GFNT_BdfState * state,
    const uint8_t * line, size_t length, size_t offset, GFNT_Error * error) {
  size_t at = 0;
  size_t end = length;
  uint8_t * row;

  while (at < end && gfnt_bdf_space(line[at])) {
    ++at;
  }
  while (end > at && gfnt_bdf_space(line[end - 1])) {
    --end;
  }
  if (state->rows_read >= state->record.height) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_BDF, offset,
        (uint32_t)gfnt_bitmap_build_count(state->build),
        "a character whose BITMAP has more rows than its BBX");
  }
  if (end - at < state->stride * 2u) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_BDF, offset,
        (uint32_t)gfnt_bitmap_build_count(state->build),
        "a BITMAP row with fewer digits than the glyph is wide");
  }
  if (state->stride == 0) {
    // A `BBX 0 4 0 0` - a box with height and no width - whose BITMAP then has
    // rows. There is nothing in them to read, and there is no buffer either:
    // `gfnt_bdf_rows()` allocated none for zero bytes, so `state->rows` is NULL and
    // the arithmetic below would be `NULL + 0`, which is undefined behaviour even
    // though every compiler computes NULL. UBSan caught it in the fuzzer, on a
    // font no fixture had.
    //
    // The row is counted rather than refused, which keeps the "more rows than its
    // BBX" check above meaning something, and no reader draws anything for a glyph
    // with no width.
    state->rows_read += 1;
    return GFNT_OK;
  }
  row = state->rows + (size_t)state->rows_read * state->stride;
  for (size_t i = 0; i < state->stride; ++i) {
    unsigned high = gfnt_bdf_hex(line[at + 2u * i]);
    unsigned low = gfnt_bdf_hex(line[at + 2u * i + 1u]);

    if (high >= 16u || low >= 16u) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_BDF, offset,
          (uint32_t)gfnt_bitmap_build_count(state->build),
          "a BITMAP row holding something that is not hexadecimal");
    }
    row[i] = (uint8_t)((high << 4) | low);
  }
  state->rows_read += 1;
  return GFNT_OK;
}

/** Make room for one glyph's rows. */
static bool gfnt_bdf_rows(GFNT_BdfState * state,
    const GFNT_Allocator * allocator, size_t needed) {
  uint8_t * grown;

  if (needed <= state->row_capacity) {
    return true;
  }
  grown = allocator->realloc_fn(allocator->ctx, state->rows, needed);
  if (!grown) {
    return false;
  }
  state->rows = grown;
  state->row_capacity = needed;
  return true;
}

GFNT_Result gfnt_bdf_parse(const GFNT_Face * face, void * out_font,
    void * context,
    GFNT_Error * error) {
  // There is one of this table per face, so there is nothing to select; the
  // parameter is here because every memo parser shares one signature.
  (void)context;
  GFNT_BitmapFont * font = out_font;
  GFNT_BdfState state;
  GFNT_Reader reader;
  GFNT_Lines lines;
  GFNT_Strike * strike;
  const uint8_t * line = NULL;
  size_t length = 0;
  bool more = false;
  bool ended = false;
  GFNT_Result result;

  memset(&state, 0, sizeof state);
  state.encoding = -1;

  result = gfnt_face_table_reader(face, GFNT_TAG_BDF, &reader, error);
  if (result != GFNT_OK) {
    return result;
  }
  // CHARS is read before the builder is started where it can be - it is the one
  // count the file states - but a BDF may put it after a property list of any
  // length, so the builder starts at zero and the count is checked per glyph.
  result = gfnt_bitmap_build_start(&state.build, face, GFNT_FLAVOUR_BDF, 0,
      error);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_lines_init(&lines, &reader, face->limits.max_line_length);
  if (result != GFNT_OK) {
    gfnt_bitmap_build_finish(state.build, NULL, false);
    return gfnt_error_set(error, result, GFNT_TAG_BDF, 0, GFNT_GLYPH_NONE,
        "starting to read the file's lines");
  }

  while ((result = gfnt_lines_next(&lines, &line, &length, &more, error))
          == GFNT_OK
      && more) {
    GFNT_BdfToken keyword;
    size_t rest = 0;

    if (length == 0 || !gfnt_bdf_keyword(line, length, &keyword, &rest)) {
      continue;
    }
    // A bitmap row is hexadecimal and so is a keyword's first byte sometimes -
    // `ENDCHAR` begins with an E, and so does a row of 0xE0 bytes. The state
    // decides which this is, and the order of these two tests is what keeps a
    // row named `ENDCHAR` from ending the glyph: inside a BITMAP, only ENDCHAR
    // ends it, and everything else is a row.
    if (state.in_bitmap) {
      if (gfnt_bdf_is(&keyword, "ENDCHAR")) {
        result = gfnt_bdf_end_char(&state, lines.offset, error);
        if (result != GFNT_OK) {
          break;
        }
        continue;
      }
      result = gfnt_bdf_bitmap_row(&state, line, length, lines.offset, error);
      if (result != GFNT_OK) {
        break;
      }
      continue;
    }

    if (gfnt_bdf_is(&keyword, "STARTCHAR")) {
      GFNT_BdfToken name;

      if (state.in_char) {
        result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_BDF,
            lines.offset, (uint32_t)gfnt_bitmap_build_count(state.build),
            "a STARTCHAR inside a character that never ended");
        break;
      }
      gfnt_bdf_value(line, length, rest, &name);
      state.in_char = true;
      state.have_bbx = false;
      state.have_advance = false;
      state.rows_read = 0;
      state.encoding = -1;
      state.name = name.length ? name.text : NULL;
      state.name_length = name.length;
      state.record = (GFNT_BitmapRecord) {0};
      continue;
    }
    if (gfnt_bdf_is(&keyword, "ENCODING")) {
      int32_t value = 0;
      size_t at = rest;

      if (!gfnt_bdf_number(line, length, &at, &value)) {
        result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_BDF,
            lines.offset, GFNT_GLYPH_NONE, "an ENCODING with no number");
        break;
      }
      // -1 is the format's "this glyph has no character", and a second number
      // after it is the Adobe Standard Encoding position, which this library
      // does not use: it names a glyph in an encoding, not a codepoint.
      state.encoding = value;
      continue;
    }
    if (gfnt_bdf_is(&keyword, "DWIDTH")) {
      int32_t x = 0;
      size_t at = rest;

      if (!gfnt_bdf_number(line, length, &at, &x)) {
        result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_BDF,
            lines.offset, GFNT_GLYPH_NONE, "a DWIDTH with no number");
        break;
      }
      state.record.advance = x;
      state.have_advance = true;
      continue;
    }
    if (gfnt_bdf_is(&keyword, "BBX")) {
      int32_t w = 0;
      int32_t h = 0;
      int32_t x = 0;
      int32_t y = 0;
      size_t at = rest;

      if (!gfnt_bdf_number(line, length, &at, &w)
          || !gfnt_bdf_number(line, length, &at, &h)
          || !gfnt_bdf_number(line, length, &at, &x)
          || !gfnt_bdf_number(line, length, &at, &y)) {
        result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_BDF,
            lines.offset, GFNT_GLYPH_NONE, "a BBX without its four numbers");
        break;
      }
      if (w < 0 || h < 0) {
        result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_BDF,
            lines.offset, GFNT_GLYPH_NONE, "a BBX with a negative size");
        break;
      }
      state.record.width = (uint32_t)w;
      state.record.height = (uint32_t)h;
      state.record.bearing_x = x;
      // The BBX's y is the bottom of the box; this library reports the top.
      state.record.bearing_y = y + h;
      state.have_bbx = true;
      state.stride = ((size_t)w + 7u) / 8u;
      continue;
    }
    if (gfnt_bdf_is(&keyword, "BITMAP")) {
      size_t needed;

      if (!state.in_char || !state.have_bbx) {
        result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_BDF,
            lines.offset, GFNT_GLYPH_NONE,
            "a BITMAP outside a character, or before its BBX");
        break;
      }
      if (!gcu_safe_mul_size(state.stride, state.record.height, &needed)) {
        result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_BDF,
            lines.offset, GFNT_GLYPH_NONE, "a BBX whose rows do not fit a size_t");
        break;
      }
      if (needed > face->limits.max_raster_bytes) {
        result = gfnt_error_set(error, GFNT_ERR_LIMIT, GFNT_TAG_BDF,
            lines.offset, GFNT_GLYPH_NONE,
            "a character needing more than GFNT_Limits::max_raster_bytes");
        break;
      }
      if (!gfnt_bdf_rows(&state, face->allocator, needed)) {
        result = gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_BDF, lines.offset,
            GFNT_GLYPH_NONE, "a row buffer for a character");
        break;
      }
      state.in_bitmap = true;
      state.rows_read = 0;
      continue;
    }
    if (gfnt_bdf_is(&keyword, "ENDCHAR")) {
      // A character with no BITMAP at all: legal for a glyph with an empty box,
      // and gfnt_bdf_end_char() is what decides whether this one qualifies.
      result = gfnt_bdf_end_char(&state, lines.offset, error);
      if (result != GFNT_OK) {
        break;
      }
      continue;
    }
    if (gfnt_bdf_is(&keyword, "CHARS")) {
      int32_t value = 0;
      size_t at = rest;

      if (!gfnt_bdf_number(line, length, &at, &value) || value < 0) {
        result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_BDF,
            lines.offset, GFNT_GLYPH_NONE, "a CHARS with no count");
        break;
      }
      if ((size_t)value > face->limits.max_glyphs) {
        result = gfnt_error_set(error, GFNT_ERR_LIMIT, GFNT_TAG_BDF,
            lines.offset, GFNT_GLYPH_NONE,
            "more characters than GFNT_Limits::max_glyphs");
        break;
      }
      state.chars_claimed = (size_t)value;
      state.have_chars = true;
      continue;
    }
    if (gfnt_bdf_is(&keyword, "FONTBOUNDINGBOX")) {
      int32_t w = 0;
      int32_t h = 0;
      int32_t x = 0;
      int32_t y = 0;
      size_t at = rest;

      if (!gfnt_bdf_number(line, length, &at, &w)
          || !gfnt_bdf_number(line, length, &at, &h)
          || !gfnt_bdf_number(line, length, &at, &x)
          || !gfnt_bdf_number(line, length, &at, &y)) {
        result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_BDF,
            lines.offset, GFNT_GLYPH_NONE,
            "a FONTBOUNDINGBOX without its four numbers");
        break;
      }
      state.box_height = h;
      state.box_offset_y = y;
      state.have_box = true;
      continue;
    }
    if (gfnt_bdf_is(&keyword, "ENDFONT")) {
      ended = true;
      break;
    }

    // Everything else is a property, wanted or not. The four this library reads
    // are the two that state the baseline, the one that states the pixel size,
    // and the names.
    {
      GFNT_BdfToken value;
      int32_t number = 0;
      size_t at = rest;

      if (gfnt_bdf_is(&keyword, "FONT_ASCENT")
          && gfnt_bdf_number(line, length, &at, &number)) {
        state.font_ascent = number;
        state.have_font_ascent = true;
        continue;
      }
      at = rest;
      if (gfnt_bdf_is(&keyword, "FONT_DESCENT")
          && gfnt_bdf_number(line, length, &at, &number)) {
        state.font_descent = number;
        state.have_font_descent = true;
        continue;
      }
      at = rest;
      if (gfnt_bdf_is(&keyword, "PIXEL_SIZE")
          && gfnt_bdf_number(line, length, &at, &number) && number > 0) {
        state.pixel_size = number;
        state.have_pixel_size = true;
        continue;
      }
      at = rest;
      if (gfnt_bdf_is(&keyword, "DEFAULT_CHAR")
          && gfnt_bdf_number(line, length, &at, &number) && number >= 0) {
        gfnt_bitmap_build_font(state.build)->has_default_char = true;
        gfnt_bitmap_build_font(state.build)->default_char = (uint32_t)number;
        continue;
      }
      if (gfnt_bdf_is(&keyword, "FAMILY_NAME")
          || gfnt_bdf_is(&keyword, "COPYRIGHT")
          || gfnt_bdf_is(&keyword, "WEIGHT_NAME")
          || gfnt_bdf_is(&keyword, "FONT")) {
        GFNT_BitmapFont * built = gfnt_bitmap_build_font(state.build);
        size_t stored = GFNT_BITMAP_NO_STRING;

        gfnt_bdf_value(line, length, rest, &value);
        if (value.length == 0) {
          continue;
        }
        result = gfnt_bitmap_build_string(state.build, value.text, value.length,
            &stored, error);
        if (result != GFNT_OK) {
          break;
        }
        if (gfnt_bdf_is(&keyword, "FAMILY_NAME")) {
          built->family = stored;
        }
        else if (gfnt_bdf_is(&keyword, "COPYRIGHT")) {
          built->copyright = stored;
        }
        else if (gfnt_bdf_is(&keyword, "WEIGHT_NAME")) {
          built->weight = stored;
        }
        else {
          // FONT is the XLFD name, which is the closest thing a BDF has to a
          // PostScript name: one string naming the whole font.
          built->full_name = stored;
        }
        continue;
      }
    }
  }

  if (result == GFNT_OK && state.in_char) {
    result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_BDF, lines.offset,
        (uint32_t)gfnt_bitmap_build_count(state.build),
        "a character that never reached its ENDCHAR");
  }
  if (result == GFNT_OK && !ended) {
    // A truncated BDF is corrupt rather than short: the glyphs read so far are
    // fine, and the font is still one a reader cannot know the end of.
    result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_BDF, lines.offset,
        GFNT_GLYPH_NONE, "a BDF that ends without ENDFONT");
  }
  if (result == GFNT_OK && !state.have_box) {
    // FONTBOUNDINGBOX is required by the specification, and it is the fallback
    // baseline when a font states no FONT_ASCENT: without it there is nothing to
    // fall back to, so a font that omits it is refused rather than given a
    // baseline this library made up.
    result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_BDF, 0,
        GFNT_GLYPH_NONE, "a BDF with no FONTBOUNDINGBOX");
  }
  if (result == GFNT_OK && gfnt_bitmap_build_count(state.build) == 0) {
    result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_BDF, 0,
        GFNT_GLYPH_NONE, "a BDF with no characters in it");
  }
  if (result == GFNT_OK && state.have_chars
      && gfnt_bitmap_build_count(state.build) != state.chars_claimed) {
    // M12's shape in a text format: the file states a count and then states the
    // characters, and a disagreement means one of the two was edited. Refusing is
    // right here rather than taking the minimum, because there is no second table
    // to cross-check and the glyph *order* is what a caller indexes by.
    result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_BDF, 0,
        (uint32_t)gfnt_bitmap_build_count(state.build),
        "a BDF with more or fewer characters than its CHARS states");
  }

  face->allocator->free_fn(face->allocator->ctx, state.rows);
  if (result != GFNT_OK) {
    gfnt_bitmap_build_finish(state.build, NULL, false);
    return result;
  }

  strike = gfnt_bitmap_build_strike(state.build);
  strike->index = 0;
  // The baseline: FONT_ASCENT and FONT_DESCENT when the font states them, and
  // otherwise the font bounding box, whose own y *is* measured from the baseline.
  // So a BDF always states one, which is what separates it from PSF and `.hex`.
  strike->ascent = state.have_font_ascent ? state.font_ascent
      : state.box_height + state.box_offset_y;
  strike->descent = state.have_font_descent ? -state.font_descent
      : state.box_offset_y;
  if (state.have_pixel_size) {
    strike->ppem_y = (uint32_t)state.pixel_size;
  }
  else {
    int32_t height = strike->ascent - strike->descent;

    strike->ppem_y = height > 0 ? (uint32_t)height : 1u;
  }
  strike->ppem_x = strike->ppem_y;
  gfnt_bitmap_build_font(state.build)->states_encoding = true;
  gfnt_bitmap_build_font(state.build)->states_baseline = true;
  gfnt_bitmap_build_finish(state.build, font, true);
  return GFNT_OK;
}
