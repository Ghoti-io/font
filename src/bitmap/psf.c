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
 * PSF 1 and 2: a Linux console font, which is a header and then the glyphs.
 *
 * documentation/design.md sections 7.1 and 7.5. Every glyph is the same size -
 * that is what a character generator is - so there are no per-glyph metrics at
 * all, and the file is a header, `length` fixed-size cells, and an optional
 * table saying which characters each cell stands for.
 *
 * Four things about it are worth writing down, because three of them are traps:
 *
 *  - **Version 1 states no glyph count.** It is 256, or 512 when one bit of the
 *    mode byte says so, and the file would be the same length either way for a
 *    height of 8. So the count comes from the mode and is then checked against
 *    what the file can hold.
 *  - **The two versions spell the Unicode table differently**: version 1 in
 *    UCS-2 little-endian terminated by `0xFFFF`, version 2 in UTF-8 terminated
 *    by `0xFF`. Same idea, no shared bytes.
 *  - **Without that table a PSF states no characters.** Its glyph indices are
 *    positions in a console's font, not codepoints, and this library says so
 *    (::GFNT_BitmapFont::states_encoding) rather than mapping index to codepoint
 *    and inventing a font that claims to hold U+0001.
 *  - **It states no baseline**, like `.hex`, so the box is reported where it is.
 *
 * Reference: the Linux kernel's `Documentation/admin-guide/` PSF description and
 * `psf(5)` from `kbd`; `psf2.h` in `kbd`'s sources for the header layout.
 */

#include <ghoti.io/cutil/safemath.h>
#include <ghoti.io/font/macros.h>
#include "../sfnt/sfnt.h"
#include "bitmap.h"

/** PSF 1's two magic bytes. */
#define GFNT_PSF1_MAGIC0 0x36u
#define GFNT_PSF1_MAGIC1 0x04u
/** PSF 1's mode bits: 512 glyphs, a Unicode table, sequences in it. */
#define GFNT_PSF1_MODE512 0x01u
#define GFNT_PSF1_HAS_TABLE 0x02u
#define GFNT_PSF1_HAS_SEQUENCES 0x04u
/** Everything above these bits is a mode this format does not define. */
#define GFNT_PSF1_MODE_MASK 0x07u
/** PSF 1's terminators, in the UCS-2 values of its table. */
#define GFNT_PSF1_SEPARATOR 0xFFFEu
#define GFNT_PSF1_TERMINATOR 0xFFFFu

/** PSF 2's four magic bytes. */
#define GFNT_PSF2_MAGIC0 0x72u
#define GFNT_PSF2_MAGIC1 0xB5u
#define GFNT_PSF2_MAGIC2 0x4Au
#define GFNT_PSF2_MAGIC3 0x86u
/** PSF 2's one flag: the Unicode table is present. */
#define GFNT_PSF2_HAS_TABLE 0x01u
/** PSF 2's terminators, as bytes in its UTF-8 table. */
#define GFNT_PSF2_SEPARATOR 0xFEu
#define GFNT_PSF2_TERMINATOR 0xFFu
/** The header PSF 2 defines; `headersize` may be larger and is then padding. */
#define GFNT_PSF2_HEADER 32u

/** PSF 1's only width. A console cell is eight pixels across. */
#define GFNT_PSF1_WIDTH 8u

bool gfnt_psf_looks_like(const GFNT_Reader * blob) {
  uint8_t bytes[4] = {0, 0, 0, 0};

  for (size_t i = 0; i < 2; ++i) {
    if (gfnt_reader_u8_at(blob, i, &bytes[i]) != GFNT_OK) {
      return false;
    }
  }
  if (bytes[0] == GFNT_PSF1_MAGIC0 && bytes[1] == GFNT_PSF1_MAGIC1) {
    return true;
  }
  for (size_t i = 2; i < 4; ++i) {
    if (gfnt_reader_u8_at(blob, i, &bytes[i]) != GFNT_OK) {
      return false;
    }
  }
  return bytes[0] == GFNT_PSF2_MAGIC0 && bytes[1] == GFNT_PSF2_MAGIC1
      && bytes[2] == GFNT_PSF2_MAGIC2 && bytes[3] == GFNT_PSF2_MAGIC3;
}

/**
 * One codepoint from a UTF-8 sequence, as PSF 2's table spells them.
 *
 * Written here rather than reached for from `unicode`: this library depends on
 * `cutil` and on nothing else (design.md section 4), and what is needed is four
 * lines of the encoding rather than any of its semantics. The overlong forms and
 * the surrogates are refused, because a table that encodes U+0041 in two bytes
 * would map two different sequences to one character and a lookup would then
 * depend on which was written.
 *
 * @param reader The table's reader, positioned at the sequence.
 * @param out_code Receives the codepoint.
 * @return ::GFNT_OK, or ::GFNT_ERR_CORRUPT.
 */
static GFNT_Result gfnt_psf_utf8(GFNT_Reader * reader, uint32_t * out_code) {
  uint8_t lead = 0;
  unsigned extra;
  uint32_t code;
  uint32_t lowest;

  if (gfnt_read_u8(reader, &lead) != GFNT_OK) {
    return GFNT_ERR_CORRUPT;
  }
  if (lead < 0x80u) {
    *out_code = lead;
    return GFNT_OK;
  }
  if ((lead & 0xE0u) == 0xC0u) {
    extra = 1;
    code = lead & 0x1Fu;
    lowest = 0x80u;
  }
  else if ((lead & 0xF0u) == 0xE0u) {
    extra = 2;
    code = lead & 0x0Fu;
    lowest = 0x800u;
  }
  else if ((lead & 0xF8u) == 0xF0u) {
    extra = 3;
    code = lead & 0x07u;
    lowest = 0x10000u;
  }
  else {
    return GFNT_ERR_CORRUPT;
  }
  for (unsigned i = 0; i < extra; ++i) {
    uint8_t next = 0;

    if (gfnt_read_u8(reader, &next) != GFNT_OK || (next & 0xC0u) != 0x80u) {
      return GFNT_ERR_CORRUPT;
    }
    code = (code << 6) | (uint32_t)(next & 0x3Fu);
  }
  if (code < lowest || code > 0x10FFFFu
      || (code >= 0xD800u && code <= 0xDFFFu)) {
    return GFNT_ERR_CORRUPT;
  }
  *out_code = code;
  return GFNT_OK;
}

/**
 * Read one glyph's entry of a Unicode table and map every character in it.
 *
 * An entry is a list of single characters, then optionally `0xFE`-separated
 * *sequences* of them - a cell that stands for a base and a combining mark
 * together. A sequence is not a codepoint and cannot go in a codepoint-to-glyph
 * map, so it is read past rather than recorded; refusing the font over one would
 * refuse fonts the console draws every day.
 *
 * @param build The builder.
 * @param reader The table's reader, positioned at this glyph's entry.
 * @param glyph Which glyph the entry belongs to.
 * @param version 1 or 2, which decides how a character is spelled.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK, or ::GFNT_ERR_CORRUPT for an entry that never terminates.
 */
static GFNT_Result gfnt_psf_table_entry(GFNT_BitmapBuild * build,
    GFNT_Reader * reader, uint32_t glyph, unsigned version,
    GFNT_Error * error) {
  bool in_sequence = false;

  for (;;) {
    uint32_t code = 0;
    size_t at = gfnt_reader_tell(reader);

    if (version == 1) {
      uint16_t value = 0;

      if (gfnt_read_u16_order(reader, GFNT_ORDER_LSB_FIRST, &value)
          != GFNT_OK) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PSF, at, glyph,
            "a Unicode table entry that ends before its terminator");
      }
      if (value == GFNT_PSF1_TERMINATOR) {
        return GFNT_OK;
      }
      if (value == GFNT_PSF1_SEPARATOR) {
        in_sequence = true;
        continue;
      }
      code = value;
    }
    else {
      uint8_t peek = 0;

      if (gfnt_reader_u8_at(reader, at, &peek) != GFNT_OK) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PSF, at, glyph,
            "a Unicode table entry that ends before its terminator");
      }
      if (peek == GFNT_PSF2_TERMINATOR) {
        return gfnt_reader_skip(reader, 1) == GFNT_OK ? GFNT_OK
            : GFNT_ERR_CORRUPT;
      }
      if (peek == GFNT_PSF2_SEPARATOR) {
        in_sequence = true;
        if (gfnt_reader_skip(reader, 1) != GFNT_OK) {
          return GFNT_ERR_CORRUPT;
        }
        continue;
      }
      if (gfnt_psf_utf8(reader, &code) != GFNT_OK) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PSF, at, glyph,
            "a Unicode table entry holding bytes that are not UTF-8");
      }
    }
    if (!in_sequence) {
      GFNT_Result result = gfnt_bitmap_build_map(build, code, glyph, error);

      if (result != GFNT_OK) {
        return result;
      }
    }
  }
}

GFNT_Result gfnt_psf_parse(const GFNT_Face * face, void * out_font,
    GFNT_Error * error) {
  GFNT_BitmapFont * font = out_font;
  GFNT_BitmapBuild * build = NULL;
  GFNT_Reader reader;
  GFNT_Strike * strike;
  unsigned version;
  uint32_t glyph_count = 0;
  uint32_t width;
  uint32_t height = 0;
  uint32_t charsize = 0;
  size_t stride;
  size_t data_offset;
  bool has_table = false;
  GFNT_Result result;

  result = gfnt_face_table_reader(face, GFNT_TAG_PSF, &reader, error);
  if (result != GFNT_OK) {
    return result;
  }

  {
    uint8_t magic0 = 0;
    uint8_t magic1 = 0;

    if (gfnt_read_u8(&reader, &magic0) != GFNT_OK
        || gfnt_read_u8(&reader, &magic1) != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_FORMAT, GFNT_TAG_PSF, 0,
          GFNT_GLYPH_NONE, "too short to hold a PSF header");
    }
    version = (magic0 == GFNT_PSF1_MAGIC0 && magic1 == GFNT_PSF1_MAGIC1) ? 1 : 2;
  }

  if (version == 1) {
    uint8_t mode = 0;
    uint8_t cell = 0;

    if (gfnt_read_u8(&reader, &mode) != GFNT_OK
        || gfnt_read_u8(&reader, &cell) != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PSF, 2,
          GFNT_GLYPH_NONE, "a PSF 1 header cut short");
    }
    if ((mode & (uint8_t)~GFNT_PSF1_MODE_MASK) != 0) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PSF, 2,
          GFNT_GLYPH_NONE, "a PSF 1 mode byte with bits the format reserves");
    }
    width = GFNT_PSF1_WIDTH;
    height = cell;
    charsize = cell;
    // Nothing in a version 1 file says how many glyphs it has. One bit of the
    // mode says 512 rather than 256, and that is the whole of the information.
    glyph_count = (mode & GFNT_PSF1_MODE512) ? 512u : 256u;
    has_table = (mode & (GFNT_PSF1_HAS_TABLE | GFNT_PSF1_HAS_SEQUENCES)) != 0;
    data_offset = 4;
  }
  else {
    uint32_t psf_version = 0;
    uint32_t header_size = 0;
    uint32_t flags = 0;

    if (gfnt_reader_seek(&reader, 4) != GFNT_OK
        || gfnt_read_u32_order(&reader, GFNT_ORDER_LSB_FIRST, &psf_version)
            != GFNT_OK
        || gfnt_read_u32_order(&reader, GFNT_ORDER_LSB_FIRST, &header_size)
            != GFNT_OK
        || gfnt_read_u32_order(&reader, GFNT_ORDER_LSB_FIRST, &flags) != GFNT_OK
        || gfnt_read_u32_order(&reader, GFNT_ORDER_LSB_FIRST, &glyph_count)
            != GFNT_OK
        || gfnt_read_u32_order(&reader, GFNT_ORDER_LSB_FIRST, &charsize)
            != GFNT_OK
        || gfnt_read_u32_order(&reader, GFNT_ORDER_LSB_FIRST, &height)
            != GFNT_OK
        || gfnt_read_u32_order(&reader, GFNT_ORDER_LSB_FIRST, &width)
            != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PSF, 4,
          GFNT_GLYPH_NONE, "a PSF 2 header cut short");
    }
    if (psf_version != 0) {
      return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_PSF, 4,
          GFNT_GLYPH_NONE, "a PSF 2 version this library does not read");
    }
    if (header_size < GFNT_PSF2_HEADER) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PSF, 8,
          GFNT_GLYPH_NONE,
          "a PSF 2 header smaller than the fields it has to hold");
    }
    has_table = (flags & GFNT_PSF2_HAS_TABLE) != 0;
    data_offset = header_size;
  }

  if (width == 0 || height == 0) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PSF, 0,
        GFNT_GLYPH_NONE, "a PSF glyph with no pixels in one direction");
  }
  stride = (width + 7u) / 8u;
  // The two sizes the header states have to agree, and version 1 has only one of
  // them: a cell is `charsize` bytes and a glyph is `height` rows of `stride`.
  // A file where they disagree is not one where a reader may pick - it is a file
  // whose glyph data is a different length than the header says, and every glyph
  // after the first would be read from the wrong place.
  {
    size_t expected;

    if (!gcu_safe_mul_size(stride, height, &expected)
        || expected != (size_t)charsize) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PSF, 0,
          GFNT_GLYPH_NONE,
          "a PSF whose bytes per glyph is not its height times its width");
    }
  }

  result = gfnt_bitmap_build_start(&build, face, GFNT_FLAVOUR_PSF, glyph_count,
      error);
  if (result != GFNT_OK) {
    return result;
  }

  for (uint32_t glyph = 0; glyph < glyph_count; ++glyph) {
    const uint8_t * rows = NULL;
    size_t at;
    GFNT_BitmapRecord record;

    if (!gcu_safe_mul_size(glyph, charsize, &at)
        || !gcu_safe_add_size(at, data_offset, &at)
        || gfnt_reader_seek(&reader, at) != GFNT_OK
        || gfnt_read_bytes(&reader, charsize, &rows) != GFNT_OK) {
      gfnt_bitmap_build_finish(build, NULL, false);
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_PSF, data_offset,
          glyph, "a PSF that ends before the glyph its header promised");
    }
    record = (GFNT_BitmapRecord) {
      .width = width,
      .height = height,
      .bearing_x = 0,
      // No baseline: a console font's cell is the line. The box is reported with
      // its top row at the ascent line, and states_baseline says why.
      .bearing_y = (int32_t)height,
      .advance = (int32_t)width,
    };
    result = gfnt_bitmap_build_glyph(build, rows, stride, GFNT_ORDER_MSB_FIRST,
        GFNT_ORDER_MSB_FIRST, 1, &record, NULL, 0, error);
    if (result != GFNT_OK) {
      gfnt_bitmap_build_finish(build, NULL, false);
      return result;
    }
  }

  if (has_table) {
    for (uint32_t glyph = 0; glyph < glyph_count; ++glyph) {
      result = gfnt_psf_table_entry(build, &reader, glyph, version, error);
      if (result != GFNT_OK) {
        gfnt_bitmap_build_finish(build, NULL, false);
        return result;
      }
    }
  }

  strike = gfnt_bitmap_build_strike(build);
  strike->index = 0;
  strike->ppem_x = height;
  strike->ppem_y = height;
  strike->ascent = (int32_t)height;
  strike->descent = 0;
  gfnt_bitmap_build_font(build)->states_encoding = has_table;
  gfnt_bitmap_build_font(build)->states_baseline = false;
  gfnt_bitmap_build_finish(build, font, true);
  return GFNT_OK;
}
