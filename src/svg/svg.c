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
 * The `SVG ` table reader. See `svg.h` in the public headers.
 *
 * Header: version u16 (0), offset u32 to the document list. The list: count u16, then records
 * of start glyph u16, end glyph u16, document offset u32 and length u32, the offset counted
 * from the list's own start. Every read goes through the checked reader of the table.
 */

#include <ghoti.io/font/svg.h>
#include <string.h>
#include "../blob/gzip.h"
#include "../reader/reader.h"
#include "../sfnt/sfnt.h"

#define GFNT_TAG_SVG GFNT_TAG('S', 'V', 'G', ' ')
#define SVG_RECORD 12u

bool gfnt_face_has_svg(const GFNT_Face * face) {
  return face && gfnt_face_has_table(face, GFNT_TAG_SVG);
}

/** The table's bytes: the same extent ::svg_open() puts its reader over. */
static const uint8_t * bytes_of(const GFNT_Face * face) {
  const uint8_t * data = NULL;

  gfnt_face_table_data(face, GFNT_TAG_SVG, &data, NULL);
  return data;
}

/** The table, the document list's offset and the number of records in it. */
static GFNT_Result svg_open(const GFNT_Face * face, GFNT_Reader * table, size_t * list,
    size_t * count, GFNT_Error * error) {
  GFNT_Result r;
  uint16_t version = 0;
  uint32_t offset = 0;
  uint16_t n = 0;

  if (!face) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE, "no face");
  }
  r = gfnt_face_table_reader(face, GFNT_TAG_SVG, table, error);
  if (r != GFNT_OK) {
    return r;
  }
  table->error = NULL;
  if (gfnt_reader_u16_at(table, 0, &version) != GFNT_OK
      || gfnt_reader_u32_at(table, 2, &offset) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_SVG, 0, GFNT_GLYPH_NONE,
        "the SVG header is cut short");
  }
  if (version != 0) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_SVG, 0, GFNT_GLYPH_NONE,
        "an SVG table of a version other than 0");
  }
  if (gfnt_reader_u16_at(table, offset, &n) != GFNT_OK
      || (size_t)offset + 2 + (size_t)n * SVG_RECORD > table->length) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_SVG, offset, GFNT_GLYPH_NONE,
        "the SVG document list is not inside the table");
  }
  *list = offset;
  *count = n;
  return GFNT_OK;
}

/**
 * The record at @p index, its document found from @p bytes (the table, from
 * ::gfnt_face_table_data()) which the checked reader @p table is over.
 */
static GFNT_Result svg_record(const GFNT_Reader * table, const uint8_t * bytes, size_t list,
    size_t index, GFNT_SvgDocument * out, GFNT_Error * error) {
  const size_t at = list + 2 + index * SVG_RECORD;
  uint16_t first = 0;
  uint16_t last = 0;
  uint32_t offset = 0;
  uint32_t length = 0;

  if (gfnt_reader_u16_at(table, at, &first) != GFNT_OK
      || gfnt_reader_u16_at(table, at + 2, &last) != GFNT_OK
      || gfnt_reader_u32_at(table, at + 4, &offset) != GFNT_OK
      || gfnt_reader_u32_at(table, at + 8, &length) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_SVG, at, GFNT_GLYPH_NONE,
        "an SVG document record is cut short");
  }
  if (last < first) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_SVG, at, GFNT_GLYPH_NONE,
        "an SVG document serves a range that ends before it starts");
  }
  if ((uint64_t)list + offset + length > table->length) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_SVG, at, GFNT_GLYPH_NONE,
        "an SVG document is not inside the table");
  }
  out->first_glyph = first;
  out->last_glyph = last;
  out->data = bytes + list + offset;
  out->length = length;
  out->compressed = length >= 3 && out->data[0] == 0x1F && out->data[1] == 0x8B
      && out->data[2] == 0x08;
  return GFNT_OK;
}

GFNT_Result gfnt_face_svg_document_count(const GFNT_Face * face, size_t * out_count,
    GFNT_Error * error) {
  GFNT_Reader table;
  size_t list;
  size_t count;
  GFNT_Result r;

  if (!out_count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE, "nowhere to put the count");
  }
  r = svg_open(face, &table, &list, &count, error);
  if (r == GFNT_OK) {
    *out_count = count;
  }
  return r;
}

GFNT_Result gfnt_face_svg_document_at(const GFNT_Face * face, size_t index,
    GFNT_SvgDocument * out, GFNT_Error * error) {
  GFNT_Reader table;
  size_t list;
  size_t count;
  GFNT_Result r;

  if (!out) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE, "nowhere to put the document");
  }
  r = svg_open(face, &table, &list, &count, error);
  if (r != GFNT_OK) {
    return r;
  }
  if (index >= count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_SVG, 0, GFNT_GLYPH_NONE,
        "a document index past the end of the list");
  }
  return svg_record(&table, bytes_of(face), list, index, out, error);
}

GFNT_Result gfnt_face_glyph_svg(const GFNT_Face * face, uint32_t glyph, GFNT_SvgDocument * out,
    bool * out_found, GFNT_Error * error) {
  GFNT_Reader table;
  size_t list;
  size_t count;
  size_t lo = 0;
  size_t hi;
  GFNT_Result r;

  if (!out || !out_found) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "nowhere to put the document");
  }
  *out_found = false;
  r = svg_open(face, &table, &list, &count, error);
  if (r != GFNT_OK) {
    return r;
  }
  hi = count;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const size_t at = list + 2 + mid * SVG_RECORD;
    uint16_t first = 0;
    uint16_t last = 0;

    if (gfnt_reader_u16_at(&table, at, &first) != GFNT_OK
        || gfnt_reader_u16_at(&table, at + 2, &last) != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_SVG, at, GFNT_GLYPH_NONE,
          "an SVG document record is cut short");
    }
    if (glyph < first) {
      hi = mid;
    }
    else if (glyph > last) {
      lo = mid + 1;
    }
    else {
      r = svg_record(&table, bytes_of(face), list, mid, out, error);
      if (r == GFNT_OK) {
        *out_found = true;
      }
      return r;
    }
  }
  return GFNT_OK;
}

GFNT_Result gfnt_svg_document_inflate(const GFNT_Face * face, const GFNT_SvgDocument * document,
    const GFNT_Allocator * allocator, GFNT_Blob ** out_blob, GFNT_Error * error) {
  GFNT_Blob * raw = NULL;
  GFNT_Result r;

  if (!face || !document || !out_blob || (!document->data && document->length)) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no face, document or place for the blob");
  }
  r = gfnt_blob_create_memory(document->data, document->length, GFNT_BLOB_BORROWED, NULL, NULL,
      &raw, error);
  if (r != GFNT_OK) {
    return r;
  }
  if (!document->compressed) {
    *out_blob = raw;
    return GFNT_OK;
  }
  r = gfnt_gzip_inflate(raw, &face->limits, allocator, out_blob, error);
  gfnt_blob_destroy(raw);
  return r;
}
