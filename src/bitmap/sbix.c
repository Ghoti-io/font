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
 * `sbix`: see sbix.h.
 */

#include <ghoti.io/cutil/safemath.h>
#include <ghoti.io/font/metrics.h>
#include <string.h>
#include "../sfnt/sfnt.h"
#include "colorstrike.h"
#include "sbix.h"

/** Bytes in the header before the strike offsets: version, flags, count. */
#define GFNT_SBIX_HEADER_BYTES 8u
/** Bytes in a strike's header before its glyph offsets: ppem and ppi. */
#define GFNT_SBIX_STRIKE_HEADER_BYTES 4u
/** Bytes in a glyph record before its data: two origin offsets and a type. */
#define GFNT_SBIX_RECORD_HEADER_BYTES 8u

#define GFNT_TYPE_PNG GFNT_TAG('p', 'n', 'g', ' ')
#define GFNT_TYPE_JPEG GFNT_TAG('j', 'p', 'g', ' ')
#define GFNT_TYPE_TIFF GFNT_TAG('t', 'i', 'f', 'f')
#define GFNT_TYPE_DUPE GFNT_TAG('d', 'u', 'p', 'e')

bool gfnt_face_has_sbix(const GFNT_Face * face) {
  return face && gfnt_face_has_table(face, GFNT_TAG_SBIX);
}

/** The table, its header checked: how many strikes, and the table to read them in. */
static GFNT_Result gfnt_sbix_open(const GFNT_Face * face, GFNT_Reader * table,
    uint32_t * out_count, GFNT_Error * error) {
  GFNT_Result result = gfnt_face_table_reader(face, GFNT_TAG_SBIX, table, error);
  uint16_t version = 0;
  uint16_t flags = 0;
  size_t need;

  if (result != GFNT_OK) {
    return result;
  }
  if (gfnt_reader_u16_at(table, 0, &version) != GFNT_OK
      || gfnt_reader_u16_at(table, 2, &flags) != GFNT_OK
      || gfnt_reader_u32_at(table, 4, out_count) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_SBIX, 0,
        GFNT_GLYPH_NONE, "an sbix shorter than its own header");
  }
  if (version != GFNT_SBIX_VERSION) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_SBIX, 0,
        GFNT_GLYPH_NONE, "an sbix version this library does not read");
  }
  if (*out_count > face->limits.max_strikes) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, GFNT_TAG_SBIX, 4,
        GFNT_GLYPH_NONE, "more sbix strikes than GFNT_Limits::max_strikes");
  }
  // The offsets array has to fit; it is the only thing the count is checked
  // against, since a strike's own extent is checked where it is read.
  if (!gcu_safe_mul_size(*out_count, 4u, &need)
      || !gcu_safe_add_size(need, GFNT_SBIX_HEADER_BYTES, &need)
      || need > gfnt_reader_remaining(table) + gfnt_reader_tell(table)) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_SBIX, 4,
        GFNT_GLYPH_NONE, "an sbix whose strike offsets run past the table");
  }
  (void)flags;   // bit 0 is "always set" and bit 1 asks for outlines under the picture
  return GFNT_OK;
}

/** Where strike @p index starts, from the table's start; its header read. */
static GFNT_Result gfnt_sbix_strike(const GFNT_Face * face,
    const GFNT_Reader * table, size_t index, size_t * out_start, uint16_t * out_ppem,
    uint16_t * out_ppi, GFNT_Error * error) {
  uint32_t offset = 0;
  size_t glyphs = 0;
  size_t need;
  GFNT_Result result;

  if (gfnt_reader_u32_at(table, GFNT_SBIX_HEADER_BYTES + 4u * index, &offset) != GFNT_OK
      || gfnt_reader_u16_at(table, offset, out_ppem) != GFNT_OK
      || gfnt_reader_u16_at(table, (size_t)offset + 2u, out_ppi) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_SBIX,
        GFNT_SBIX_HEADER_BYTES + 4u * index, GFNT_GLYPH_NONE,
        "an sbix strike that starts past the end of the table");
  }
  result = gfnt_face_num_glyphs(face, &glyphs, error);
  if (result != GFNT_OK) {
    return result;
  }
  // numGlyphs + 1 offsets follow the header.
  if (!gcu_safe_mul_size(glyphs + 1u, 4u, &need)
      || !gcu_safe_add_size(need, GFNT_SBIX_STRIKE_HEADER_BYTES, &need)
      || !gcu_safe_add_size(need, offset, &need)
      || need > gfnt_reader_remaining(table) + gfnt_reader_tell(table)) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_SBIX, offset,
        GFNT_GLYPH_NONE, "an sbix strike whose glyph offsets run past the table");
  }
  if (*out_ppem == 0) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_SBIX, offset,
        GFNT_GLYPH_NONE, "an sbix strike of zero pixels per em");
  }
  *out_start = offset;
  return GFNT_OK;
}

GFNT_Result gfnt_sbix_strike_count(const GFNT_Face * face, size_t * out_count,
    GFNT_Error * error) {
  GFNT_Reader table;
  uint32_t count = 0;
  GFNT_Result result = gfnt_sbix_open(face, &table, &count, error);

  if (result != GFNT_OK) {
    return result;
  }
  *out_count = count;
  return GFNT_OK;
}

GFNT_Result gfnt_sbix_strike_at(const GFNT_Face * face, size_t index,
    GFNT_Strike * out_strike, GFNT_Error * error) {
  GFNT_Reader table;
  uint32_t count = 0;
  size_t start = 0;
  uint16_t ppem = 0;
  uint16_t ppi = 0;
  GFNT_Result result = gfnt_sbix_open(face, &table, &count, error);

  if (result != GFNT_OK) {
    return result;
  }
  if (index >= count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_SBIX, 0,
        GFNT_GLYPH_NONE, "the face has no strike at that index");
  }
  result = gfnt_sbix_strike(face, &table, index, &start, &ppem, &ppi, error);
  if (result != GFNT_OK) {
    return result;
  }
  // `sbix` states no line metrics, so the ascent and descent are zero rather than a
  // baseline invented from `hhea`; ppi is the strike's resolution and nothing here
  // reads it (HarfBuzz does not either).
  *out_strike = (GFNT_Strike) {
    .index = index,
    .ppem_x = ppem,
    .ppem_y = ppem,
    .bit_depth = 32,
    .kind = GFNT_GLYPH_BITMAP_PNG,
    .ascent = 0,
    .descent = 0,
  };
  return GFNT_OK;
}

size_t gfnt_sbix_strike_for(const GFNT_Face * face, uint32_t requested) {
  GFNT_Reader table;
  uint32_t count = 0;
  size_t best = 0;
  uint32_t best_ppem = 0;

  if (gfnt_sbix_open(face, &table, &count, NULL) != GFNT_OK) {
    return 0;
  }
  for (size_t i = 0; i < count; ++i) {
    size_t start = 0;
    uint16_t ppem = 0;
    uint16_t ppi = 0;

    if (gfnt_sbix_strike(face, &table, i, &start, &ppem, &ppi, NULL) != GFNT_OK) {
      continue;
    }
    if (best_ppem == 0 || gfnt_strike_better(requested, ppem, best_ppem)) {
      best = i;
      best_ppem = ppem;
    }
  }
  return best;
}

/** A PNG's size from its header, or 0 and 0 when the header is not one. */
static void gfnt_png_size(const uint8_t * data, size_t length, uint32_t * width,
    uint32_t * height) {
  static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  uint32_t w;
  uint32_t h;

  *width = *height = 0;
  // Signature, then the IHDR chunk: a length of 13, its name, width, height.
  if (length < 24 || memcmp(data, signature, 8) != 0
      || memcmp(data + 8, "\0\0\0\r", 4) != 0 || memcmp(data + 12, "IHDR", 4) != 0) {
    return;
  }
  w = ((uint32_t)data[16] << 24) | ((uint32_t)data[17] << 16)
      | ((uint32_t)data[18] << 8) | data[19];
  h = ((uint32_t)data[20] << 24) | ((uint32_t)data[21] << 16)
      | ((uint32_t)data[22] << 8) | data[23];
  if (w > INT32_MAX || h > INT32_MAX) {
    return;
  }
  *width = w;
  *height = h;
}

GFNT_Result gfnt_sbix_find(const GFNT_Face * face, size_t strike, uint32_t glyph,
    GFNT_GlyphImage * out, GFNT_Error * error) {
  GFNT_Reader table;
  uint32_t count = 0;
  size_t start = 0;
  uint16_t ppem = 0;
  uint16_t ppi = 0;
  size_t glyphs = 0;
  size_t length;
  uint32_t target = glyph;
  GFNT_Result result = gfnt_sbix_open(face, &table, &count, error);

  if (result != GFNT_OK) {
    return result;
  }
  if (strike >= count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_SBIX, 0, glyph,
        "a strike index the face does not have");
  }
  result = gfnt_sbix_strike(face, &table, strike, &start, &ppem, &ppi, error);
  if (result != GFNT_OK) {
    return result;
  }
  result = gfnt_face_num_glyphs(face, &glyphs, error);
  if (result != GFNT_OK) {
    return result;
  }
  length = gfnt_reader_remaining(&table) + gfnt_reader_tell(&table);

  for (uint32_t hops = 0;; ++hops) {
    uint32_t begin = 0;
    uint32_t end = 0;
    size_t record;
    size_t size;
    int16_t origin_x = 0;
    int16_t origin_y = 0;
    GFNT_Tag type = 0;
    const uint8_t * bytes = NULL;
    GFNT_Reader at;

    if (target >= glyphs) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_SBIX, start, glyph,
          "an sbix dupe that names a glyph the face does not have");
    }
    if (gfnt_reader_u32_at(&table, start + 4u + 4u * (size_t)target, &begin) != GFNT_OK
        || gfnt_reader_u32_at(&table, start + 8u + 4u * (size_t)target, &end)
            != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_SBIX, start, glyph,
          "an sbix glyph offset past the end of the table");
    }
    if (end < begin) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_SBIX, start, glyph,
          "an sbix glyph whose offsets run backwards");
    }
    size = (size_t)(end - begin);
    if (size == 0 || size == GFNT_SBIX_RECORD_HEADER_BYTES) {
      // No bytes, or a record that names a type and holds no picture: the strike
      // does not carry the glyph (and a `dupe` whose target is such a glyph is the
      // same).
      return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_SBIX, start, glyph,
          "a glyph this strike does not carry");
    }
    if (size < GFNT_SBIX_RECORD_HEADER_BYTES
        || !gcu_safe_add_size(start, begin, &record)
        || record > length || size > length - record) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_SBIX, start, glyph,
          "an sbix glyph record that does not fit the table");
    }
    at = table;
    if (gfnt_reader_seek(&at, record) != GFNT_OK
        || gfnt_read_s16(&at, &origin_x) != GFNT_OK
        || gfnt_read_s16(&at, &origin_y) != GFNT_OK
        || gfnt_read_tag(&at, &type) != GFNT_OK
        || gfnt_read_bytes(&at, size - GFNT_SBIX_RECORD_HEADER_BYTES, &bytes)
            != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_SBIX, record, glyph,
          "an sbix glyph record that ends past the table");
    }
    if (type == GFNT_TYPE_DUPE) {
      // The record's data is the glyph whose picture to use, and that glyph's
      // record may be a dupe in turn. HarfBuzz follows eight hops and reads the
      // ninth glyph as empty; the bound also ends a cycle.
      if (size - GFNT_SBIX_RECORD_HEADER_BYTES < 2u) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_SBIX, record, glyph,
            "an sbix dupe with no glyph to name");
      }
      if (hops >= GFNT_SBIX_MAX_DUPE_HOPS) {
        return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_SBIX, record, glyph,
            "an sbix dupe chain longer than eight hops, or a cycle");
      }
      target = ((uint32_t)bytes[0] << 8) | bytes[1];
      continue;
    }
    if (type != GFNT_TYPE_PNG && type != GFNT_TYPE_JPEG && type != GFNT_TYPE_TIFF) {
      return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_SBIX, record, glyph,
          "an sbix graphicType this library does not read");
    }
    *out = (GFNT_GlyphImage) {
      .format = type == GFNT_TYPE_PNG ? GFNT_IMAGE_PNG
          : type == GFNT_TYPE_JPEG ? GFNT_IMAGE_JPEG : GFNT_IMAGE_TIFF,
      .data = bytes,
      .length = size - GFNT_SBIX_RECORD_HEADER_BYTES,
      .bearing_x = origin_x,
      .origin_y = origin_y,
      .advance = -1,
      .strike = {
        .index = strike,
        .ppem_x = ppem,
        .ppem_y = ppem,
        .bit_depth = 32,
        .kind = GFNT_GLYPH_BITMAP_PNG,
      },
    };
    if (type == GFNT_TYPE_PNG) {
      gfnt_png_size(bytes, out->length, &out->width, &out->height);
    }
    out->bearing_y = origin_y + (int32_t)out->height;
    return GFNT_OK;
  }
}
