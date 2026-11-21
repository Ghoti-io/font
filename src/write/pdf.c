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
 * The `W` array and the `ToUnicode` CMap a PDF writer needs for an embedded
 * subset (include/ghoti.io/font/pdf.h).
 *
 * Both are text, built in integer arithmetic so that the bytes are the same on
 * every platform, and the CMap is written to the PDF specification's own limits
 * (one hundred entries to a block, a range whose last byte does not wrap).
 */

#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/pdf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "write.h"

/* ---- widths ----------------------------------------------------------- */

typedef struct WidthRow {
  uint32_t glyph;
  int64_t width; ///< In thousandths of a 1000-unit em.
} WidthRow;

static int widthrow_cmp(const void * a, const void * b) {
  uint32_t x = ((const WidthRow *)a)->glyph;
  uint32_t y = ((const WidthRow *)b)->glyph;

  return x < y ? -1 : x > y;
}

/** @p advance font units of @p upem, in thousandths of a 1000-unit em, rounded half away from zero. */
static int64_t scaled_thousandths(int32_t advance, uint16_t upem) {
  int64_t numerator = (int64_t)advance * 1000000;
  int64_t half = (int64_t)upem / 2;

  return numerator >= 0 ? (numerator + half) / upem : -((-numerator + half) / upem);
}

static void put_text(GFNT_WBuf * b, const char * text) {
  gfnt_wbuf_bytes(b, (const uint8_t *)text, strlen(text));
}

/** A width as text: an integer when it is one, else up to three decimals with trailing zeros dropped. */
static void put_width(GFNT_WBuf * b, int64_t thousandths) {
  char text[32];
  uint64_t magnitude = thousandths < 0 ? (uint64_t)(-thousandths) : (uint64_t)thousandths;
  uint64_t whole = magnitude / 1000;
  unsigned fraction = (unsigned)(magnitude % 1000);
  int n = snprintf(text, sizeof text, "%s%llu", thousandths < 0 && magnitude ? "-" : "",
      (unsigned long long)whole);

  if (fraction) {
    n += snprintf(text + n, sizeof text - (size_t)n, ".%03u", fraction);
    while (text[n - 1] == '0') {
      text[--n] = '\0';
    }
  }
  put_text(b, text);
}

static void put_number(GFNT_WBuf * b, uint32_t value) {
  char text[16];

  snprintf(text, sizeof text, "%lu", (unsigned long)value);
  put_text(b, text);
}

/** How many consecutive glyphs from @p i have the width and id sequence of @p rows[i]. */
static size_t equal_run(const WidthRow * rows, size_t n, size_t i) {
  size_t j = i + 1;

  while (j < n && rows[j].glyph == rows[j - 1].glyph + 1 && rows[j].width == rows[i].width) {
    j++;
  }
  return j - i;
}

GFNT_Result gfnt_pdf_widths(const GFNT_Face * face, const uint32_t * glyphs, size_t count,
    const GFNT_Variation * variation, const GFNT_Limits * limits,
    const GFNT_Allocator * allocator, GFNT_Blob ** out_blob, GFNT_Error * error) {
  GFNT_Limits local;
  GFNT_WBuf b;
  WidthRow * rows;
  size_t n = 0;
  size_t i;
  uint16_t upem = 0;
  GFNT_Result result;

  if (!face || !out_blob || (!glyphs && count)) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "a face, a glyph list and a place for the result are required");
  }
  if (count == 0) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "there are no glyphs to give the widths of");
  }
  if (!limits) {
    gfnt_limits_default(&local);
    limits = &local;
  }
  if (count > limits->max_glyphs * 2) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
        "more glyphs than the glyph limit allows for");
  }
  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  result = gfnt_face_units_per_em(face, &upem, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (upem == 0) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, 0, 0, GFNT_GLYPH_NONE,
        "the face states zero units per em");
  }
  rows = allocator->malloc_fn(allocator->ctx, count * sizeof *rows);
  if (!rows) {
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE, "listing the glyphs");
  }
  for (i = 0; i < count; i++) {
    rows[i].glyph = glyphs[i];
    rows[i].width = 0;
  }
  qsort(rows, count, sizeof *rows, widthrow_cmp);
  for (i = 0; i < count; i++) {
    if (n == 0 || rows[i].glyph != rows[n - 1].glyph) {
      rows[n++].glyph = rows[i].glyph;
    }
  }
  for (i = 0; i < n; i++) {
    int32_t advance = 0;

    result = gfnt_face_glyph_advance(face, rows[i].glyph, variation, &advance, error);
    if (result != GFNT_OK) {
      allocator->free_fn(allocator->ctx, rows);
      return result;
    }
    rows[i].width = scaled_thousandths(advance, upem);
  }

  gfnt_wbuf_init(&b, allocator);
  for (i = 0; i < n;) {
    size_t run = equal_run(rows, n, i);

    if (b.length) {
      put_text(&b, " ");
    }
    if (run >= 4) {
      put_number(&b, rows[i].glyph);
      put_text(&b, " ");
      put_number(&b, rows[i + run - 1].glyph);
      put_text(&b, " ");
      put_width(&b, rows[i].width);
      i += run;
    }
    else {
      size_t j = i;

      put_number(&b, rows[i].glyph);
      put_text(&b, " [");
      for (;;) {
        put_width(&b, rows[j].width);
        j++;
        if (j >= n || rows[j].glyph != rows[j - 1].glyph + 1 || equal_run(rows, n, j) >= 4) {
          break;
        }
        put_text(&b, " ");
      }
      put_text(&b, "]");
      i = j;
    }
  }
  allocator->free_fn(allocator->ctx, rows);
  if (b.oom) {
    gfnt_wbuf_free(&b);
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE, "writing the widths");
  }
  result = gfnt_blob_create_memory(b.data, b.length, GFNT_BLOB_COPY, limits, allocator,
      out_blob, error);
  gfnt_wbuf_free(&b);
  return result;
}

/* ---- ToUnicode -------------------------------------------------------- */

/** The bytes of the UTF-16BE form of @p cp, as upper-case hex, appended. */
static void put_utf16_hex(GFNT_WBuf * b, uint32_t cp) {
  char text[16];

  if (cp >= 0x10000) {
    uint32_t v = cp - 0x10000;

    snprintf(text, sizeof text, "%04X%04X", (unsigned)(0xD800 + (v >> 10)),
        (unsigned)(0xDC00 + (v & 0x3FF)));
  }
  else {
    snprintf(text, sizeof text, "%04X", (unsigned)cp);
  }
  put_text(b, text);
}

static void put_code_hex(GFNT_WBuf * b, uint32_t code, unsigned code_bytes) {
  char text[16];

  snprintf(text, sizeof text, code_bytes == 1 ? "<%02X>" : "<%04X>", (unsigned)code);
  put_text(b, text);
}

typedef struct Piece {
  uint32_t first;
  uint32_t last;       ///< Equal to first for a bfchar.
  size_t entry;        ///< The entry of the first code.
  bool range;
} Piece;

static int entry_cmp(const void * a, const void * b) {
  uint32_t x = ((const GFNT_PdfToUnicodeEntry *)a)->code;
  uint32_t y = ((const GFNT_PdfToUnicodeEntry *)b)->code;

  return x < y ? -1 : x > y;
}

/** Whether entry @p next continues a range that began at @p start and has @p length codes. */
static bool continues_range(const GFNT_PdfToUnicodeEntry * start, size_t length,
    const GFNT_PdfToUnicodeEntry * next, unsigned code_bytes) {
  uint32_t last_code = start->code + (uint32_t)length - 1;

  if (start->count != 1 || next->count != 1 || next->code != last_code + 1) {
    return false;
  }
  /* The specification increments the last byte of the destination and of the code. */
  if (code_bytes == 2 && (next->code >> 8) != (start->code >> 8)) {
    return false;
  }
  if (next->codepoints[0] != start->codepoints[0] + (uint32_t)length) {
    return false;
  }
  return (start->codepoints[0] & 0xFF) + length <= 0xFF;
}

static void put_blocks(GFNT_WBuf * b, const GFNT_PdfToUnicodeEntry * e, const Piece * pieces,
    size_t n, bool ranges, unsigned code_bytes) {
  size_t done = 0;
  size_t i;

  while (done < n) {
    size_t block = 0;
    char line[32];

    /* The pieces of the wanted kind, a hundred to a block. */
    size_t found = 0;

    for (i = done; i < n && found < 100; i++) {
      if (pieces[i].range == ranges) {
        found++;
      }
    }
    if (found == 0) {
      return;
    }
    snprintf(line, sizeof line, "%zu %s\n", found, ranges ? "beginbfrange" : "beginbfchar");
    put_text(b, line);
    for (i = done; i < n && block < found; i++) {
      size_t k;

      if (pieces[i].range != ranges) {
        continue;
      }
      put_code_hex(b, pieces[i].first, code_bytes);
      put_text(b, " ");
      if (ranges) {
        put_code_hex(b, pieces[i].last, code_bytes);
        put_text(b, " ");
      }
      put_text(b, "<");
      for (k = 0; k < e[pieces[i].entry].count; k++) {
        put_utf16_hex(b, e[pieces[i].entry].codepoints[k]);
      }
      put_text(b, ">\n");
      block++;
    }
    put_text(b, ranges ? "endbfrange\n" : "endbfchar\n");
    done = i;
  }
}

GFNT_Result gfnt_pdf_tounicode(const GFNT_PdfToUnicodeEntry * entries, size_t count,
    unsigned code_bytes, const GFNT_Limits * limits, const GFNT_Allocator * allocator,
    GFNT_Blob ** out_blob, GFNT_Error * error) {
  GFNT_Limits local;
  GFNT_WBuf b;
  GFNT_PdfToUnicodeEntry * sorted;
  Piece * pieces;
  size_t n = 0;
  size_t npieces = 0;
  size_t i;
  size_t k;
  GFNT_Result result;

  if (!out_blob || (!entries && count) || (code_bytes != 1 && code_bytes != 2)) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "a code width of 1 or 2 bytes, the entries and a place for the result are required");
  }
  for (i = 0; i < count; i++) {
    if (entries[i].code > (code_bytes == 1 ? 0xFFu : 0xFFFFu)) {
      return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
          "a code does not fit the code width");
    }
    if (entries[i].count > GFNT_PDF_TOUNICODE_MAX_CODEPOINTS
        || (entries[i].count && !entries[i].codepoints)) {
      return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
          "an entry stands for too many codepoints, or for some that are not given");
    }
    for (k = 0; k < entries[i].count; k++) {
      uint32_t cp = entries[i].codepoints[k];

      if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
            "a codepoint is a surrogate or past U+10FFFF");
      }
    }
  }
  if (!limits) {
    gfnt_limits_default(&local);
    limits = &local;
  }
  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  sorted = allocator->malloc_fn(allocator->ctx, (count ? count : 1) * sizeof *sorted);
  pieces = allocator->malloc_fn(allocator->ctx, (count ? count : 1) * sizeof *pieces);
  if (!sorted || !pieces) {
    allocator->free_fn(allocator->ctx, sorted);
    allocator->free_fn(allocator->ctx, pieces);
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE, "listing the entries");
  }
  for (i = 0; i < count; i++) {
    if (entries[i].count) {
      sorted[n++] = entries[i];
    }
  }
  qsort(sorted, n, sizeof *sorted, entry_cmp);
  for (i = 1; i < n; i++) {
    if (sorted[i].code == sorted[i - 1].code) {
      allocator->free_fn(allocator->ctx, sorted);
      allocator->free_fn(allocator->ctx, pieces);
      return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
          "a code is given twice");
    }
  }
  for (i = 0; i < n;) {
    size_t length = 1;

    while (i + length < n && continues_range(&sorted[i], length, &sorted[i + length], code_bytes)) {
      length++;
    }
    pieces[npieces].first = sorted[i].code;
    pieces[npieces].last = sorted[i].code + (uint32_t)length - 1;
    pieces[npieces].entry = i;
    pieces[npieces].range = length > 1;
    npieces++;
    i += length;
  }

  gfnt_wbuf_init(&b, allocator);
  put_text(&b, "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
      "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
      "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
      "1 begincodespacerange\n");
  put_text(&b, code_bytes == 1 ? "<00> <FF>\n" : "<0000> <FFFF>\n");
  put_text(&b, "endcodespacerange\n");
  put_blocks(&b, sorted, pieces, npieces, false, code_bytes);
  put_blocks(&b, sorted, pieces, npieces, true, code_bytes);
  put_text(&b, "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n");
  allocator->free_fn(allocator->ctx, sorted);
  allocator->free_fn(allocator->ctx, pieces);
  if (b.oom) {
    gfnt_wbuf_free(&b);
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE, "writing the CMap");
  }
  result = gfnt_blob_create_memory(b.data, b.length, GFNT_BLOB_COPY, limits, allocator,
      out_blob, error);
  gfnt_wbuf_free(&b);
  return result;
}
