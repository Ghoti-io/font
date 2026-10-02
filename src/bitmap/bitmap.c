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
 * What the four bitmap containers share: the builder, the one place bits are
 * normalised, and the accessors a caller sees.
 *
 * documentation/design.md sections 7.1 and 7.5.
 */

#include <ghoti.io/cutil/safemath.h>
#include <ghoti.io/font/macros.h>
#include <stdlib.h>
#include <string.h>
#include "../core/buffer.h"
#include "../sfnt/sfnt.h"
#include "../tables/tables.h"
#include "bitmap.h"
#include "eblc.h"

/** How many glyph records or mappings the first allocation holds. */
#define GFNT_BITMAP_INITIAL 64u

struct GFNT_BitmapBuild {
  const GFNT_Allocator * allocator;
  GFNT_Limits limits;
  GFNT_BitmapFont font;

  GFNT_Buffer pixels;
  GFNT_Buffer text;

  GFNT_BitmapRecord * records;
  size_t record_count;
  size_t record_capacity;

  GFNT_BitmapMapping * map;
  size_t map_count;
  size_t map_capacity;

  /** One row, in the container's own layout, while it is being normalised. */
  uint8_t * row;
  size_t row_capacity;

  /**
   * One composite glyph's destination, while its components are OR-ed into it.
   *
   * Separate from `row` because a component's own rows go *through* `row` on the
   * way in, so one buffer for both would have each component overwrite the
   * composite it is being drawn into.
   */
  uint8_t * canvas;
  size_t canvas_capacity;
};

// ------------------------------------------------------------ the arenas

/**
 * Grow an array of @p size-byte elements to hold one more, doubling.
 *
 * One function for the records and the mappings, because two copies of a
 * doubling are two reallocation paths and `tools/coverage.sh` reports the one no
 * test reaches - which is the one that is wrong.
 */
static bool gfnt_bitmap_reserve(const GFNT_Allocator * allocator, void ** array,
    size_t * capacity, size_t count, size_t size) {
  size_t wanted;
  size_t bytes;
  void * grown;

  if (count < *capacity) {
    return true;
  }
  wanted = *capacity ? *capacity : GFNT_BITMAP_INITIAL;
  while (wanted <= count) {
    size_t doubled;

    if (!gcu_safe_mul_size(wanted, 2, &doubled)) {
      return false;
    }
    wanted = doubled;
  }
  if (!gcu_safe_mul_size(wanted, size, &bytes)) {
    return false;
  }
  grown = allocator->realloc_fn(allocator->ctx, *array, bytes);
  if (!grown) {
    return false;
  }
  *array = grown;
  *capacity = wanted;
  return true;
}

GFNT_Result gfnt_bitmap_build_start(GFNT_BitmapBuild ** build,
    const GFNT_Face * face, GFNT_Tag container, size_t glyph_count,
    GFNT_Error * error) {
  GFNT_BitmapBuild * made;

  if (!build || !face) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, container, 0,
        GFNT_GLYPH_NONE, "no builder, or no face to build it on");
  }
  // The claimed count is checked once, here, so that no parser has to remember
  // to: every one of these formats states how many glyphs it has, and a file
  // that states four billion is refused before an allocation is attempted with
  // the number.
  if (glyph_count > face->limits.max_glyphs) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, container, 0, GFNT_GLYPH_NONE,
        "more glyphs than GFNT_Limits::max_glyphs");
  }
  made = face->allocator->calloc_fn(face->allocator->ctx, 1, sizeof *made);
  if (!made) {
    return gfnt_error_set(error, GFNT_ERR_OOM, container, 0, GFNT_GLYPH_NONE,
        "allocating the bitmap builder");
  }
  made->allocator = face->allocator;
  made->limits = face->limits;
  gfnt_buffer_init(&made->pixels, face->allocator);
  gfnt_buffer_init(&made->text, face->allocator);
  made->font.container = container;
  made->font.family = GFNT_BITMAP_NO_STRING;
  made->font.full_name = GFNT_BITMAP_NO_STRING;
  made->font.copyright = GFNT_BITMAP_NO_STRING;
  made->font.weight = GFNT_BITMAP_NO_STRING;
  made->font.strike.bit_depth = 1;
  made->font.strike.kind = GFNT_GLYPH_BITMAP_MONO;

  // A text arena that starts with a NUL means offset 0 is the empty string, so
  // "no name" can be a distinct value rather than colliding with the first name
  // stored. GFNT_BITMAP_NO_STRING is that value, and this is what keeps it from
  // ever being a real offset.
  if (!gfnt_buffer_byte(&made->text, 0)) {
    gfnt_bitmap_build_finish(made, NULL, false);
    return gfnt_error_set(error, GFNT_ERR_OOM, container, 0, GFNT_GLYPH_NONE,
        "allocating the bitmap string arena");
  }
  *build = made;
  return GFNT_OK;
}

GFNT_Strike * gfnt_bitmap_build_strike(GFNT_BitmapBuild * build) {
  return &build->font.strike;
}

GFNT_BitmapFont * gfnt_bitmap_build_font(GFNT_BitmapBuild * build) {
  return &build->font;
}

size_t gfnt_bitmap_build_count(const GFNT_BitmapBuild * build) {
  return build->record_count;
}

GFNT_Result gfnt_bitmap_build_string(GFNT_BitmapBuild * build,
    const char * text, size_t length, size_t * out_offset,
    GFNT_Error * error) {
  // No NULL arm: every caller decides for itself whether it has a string, because
  // "no name" is a different fact from "the empty name" and the callers are the
  // ones that can tell. The arm was here and no input reached it.
  size_t at = build->text.length;

  if (!gfnt_buffer_add(&build->text, (const uint8_t *)text, length)
      || !gfnt_buffer_byte(&build->text, 0)) {
    return gfnt_error_set(error, GFNT_ERR_OOM, build->font.container, 0,
        GFNT_GLYPH_NONE, "storing a bitmap font's string");
  }
  *out_offset = at;
  return GFNT_OK;
}

// ------------------------------------------------------- the normalisation

/** Reverse the bits of a byte, so an LSB-first row becomes an MSB-first one. */
static uint8_t gfnt_bitmap_reverse(uint8_t byte) {
  uint8_t out = 0;

  for (unsigned bit = 0; bit < 8; ++bit) {
    if (byte & (uint8_t)(1u << bit)) {
      out |= (uint8_t)(0x80u >> bit);
    }
  }
  return out;
}

/**
 * Put one row into this library's layout: rows of bytes, leftmost pixel in the
 * most significant bit of the first byte.
 *
 * The two steps are independent and commute, which is why they can be two
 * statements rather than a table of four cases:
 *
 *  - **Bit order** decides which end of a byte the leftmost pixel of its eight
 *    is at, and is undone by reversing each byte.
 *  - **Byte order** decides where the first byte of a scan unit is, and matters
 *    only when a scan unit is more than one byte. A unit's bytes are reversed
 *    when the two orders *differ*: a file whose bits and bytes are both
 *    least-significant-first has already put the bytes of each unit in the order
 *    the reversal of its bits wants them.
 *
 * That last sentence is the whole of the subtlety, and getting it wrong makes a
 * 16-pixel-wide font from a little-endian machine read as two swapped halves -
 * which looks like a font bug and not like a reader bug, because every glyph is
 * still a plausible glyph.
 *
 * @param row The row, @p stride bytes, rewritten in place.
 * @param stride Its length, including the container's padding.
 * @param width How many pixels are real; the bits past it are cleared.
 * @param bit_order Which end of a byte the file starts at.
 * @param byte_order Which end of a scan unit the file starts at.
 * @param scan_unit Bytes per scan unit: 1, 2 or 4.
 */
static void gfnt_bitmap_normalise_row(uint8_t * row, size_t stride,
    uint32_t width, GFNT_ByteOrder bit_order, GFNT_ByteOrder byte_order,
    unsigned scan_unit) {
  size_t used = (width + 7u) / 8u;

  if (scan_unit > 1 && byte_order != bit_order) {
    for (size_t at = 0; at + scan_unit <= stride; at += scan_unit) {
      for (unsigned i = 0; i < scan_unit / 2; ++i) {
        uint8_t swap = row[at + i];

        row[at + i] = row[at + scan_unit - 1 - i];
        row[at + scan_unit - 1 - i] = swap;
      }
    }
  }
  if (bit_order == GFNT_ORDER_LSB_FIRST) {
    for (size_t at = 0; at < stride; ++at) {
      row[at] = gfnt_bitmap_reverse(row[at]);
    }
  }
  // The bits past the last pixel are cleared rather than kept, because two
  // faces of one design have to compare equal and one of them may have had
  // rubbish there. A PCF written by bdftopcf does not; a PCF written by
  // something else is not required not to.
  if (used > 0 && (width % 8u) != 0) {
    row[used - 1] &= (uint8_t)(0xFFu << (8u - (width % 8u)));
  }
}

GFNT_Result gfnt_bitmap_build_glyph(GFNT_BitmapBuild * build,
    const uint8_t * rows, size_t source_stride, GFNT_ByteOrder bit_order,
    GFNT_ByteOrder byte_order, unsigned scan_unit,
    const GFNT_BitmapRecord * metrics, const char * name, size_t name_length,
    GFNT_Error * error) {
  GFNT_BitmapRecord record = *metrics;
  size_t needed;
  void * array;

  // A glyph added here is one the file carries and this library read; the other
  // two states have functions of their own, so a caller cannot reach them by
  // accident.
  record.presence = GFNT_BITMAP_PRESENT;

  if (scan_unit != 1 && scan_unit != 2 && scan_unit != 4) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, build->font.container, 0,
        (uint32_t)build->record_count,
        "a scan unit that is not one, two or four bytes");
  }
  if (record.width > build->limits.max_ppem
      || record.height > build->limits.max_ppem) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, build->font.container, 0,
        (uint32_t)build->record_count,
        "a glyph wider or taller than GFNT_Limits::max_ppem");
  }
  record.stride = (record.width + 7u) / 8u;
  if (!gcu_safe_mul_size(record.stride, record.height, &needed)) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, build->font.container, 0,
        (uint32_t)build->record_count, "a glyph whose rows do not fit a size_t");
  }
  if (needed > build->limits.max_raster_bytes) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, build->font.container, 0,
        (uint32_t)build->record_count,
        "a glyph needing more than GFNT_Limits::max_raster_bytes");
  }
  // A box with no area is a glyph with no pixels, and a space is exactly that.
  // Its advance is still its own, which is the whole reason the record exists.
  if (record.width == 0 || record.height == 0 || !rows) {
    record.width = 0;
    record.height = 0;
    record.stride = 0;
    record.offset = 0;
  }
  else {
    if (source_stride < record.stride) {
      return gfnt_error_set(error, GFNT_ERR_CORRUPT, build->font.container, 0,
          (uint32_t)build->record_count,
          "rows narrower than the glyph they are supposed to hold");
    }
    if (source_stride > build->row_capacity) {
      uint8_t * grown = build->allocator->realloc_fn(build->allocator->ctx,
          build->row, source_stride);

      if (!grown) {
        return gfnt_error_set(error, GFNT_ERR_OOM, build->font.container, 0,
            (uint32_t)build->record_count, "a row buffer for a bitmap glyph");
      }
      build->row = grown;
      build->row_capacity = source_stride;
    }
    record.offset = build->pixels.length;
    if (!gfnt_buffer_grow(&build->pixels, needed)) {
      return gfnt_error_set(error, GFNT_ERR_OOM, build->font.container, 0,
          (uint32_t)build->record_count, "the bitmap pixel arena");
    }
    for (uint32_t y = 0; y < record.height; ++y) {
      memcpy(build->row, rows + (size_t)y * source_stride, source_stride);
      gfnt_bitmap_normalise_row(build->row, source_stride, record.width,
          bit_order, byte_order, scan_unit);
      if (!gfnt_buffer_add(&build->pixels, build->row, record.stride)) {
        return gfnt_error_set(error, GFNT_ERR_OOM, build->font.container, 0,
            (uint32_t)build->record_count, "the bitmap pixel arena");
      }
    }
  }

  if (name) {
    GFNT_Result result = gfnt_bitmap_build_string(build, name, name_length,
        &record.name, error);

    if (result != GFNT_OK) {
      return result;
    }
  }
  else {
    record.name = GFNT_BITMAP_NO_STRING;
  }

  array = build->records;
  if (!gfnt_bitmap_reserve(build->allocator, &array, &build->record_capacity,
          build->record_count, sizeof *build->records)) {
    return gfnt_error_set(error, GFNT_ERR_OOM, build->font.container, 0,
        (uint32_t)build->record_count, "the bitmap glyph array");
  }
  build->records = array;
  build->records[build->record_count++] = record;
  return GFNT_OK;
}

GFNT_Result gfnt_bitmap_build_canvas(GFNT_BitmapBuild * build, size_t bytes,
    uint8_t ** out_canvas, GFNT_Error * error) {
  if (bytes > build->canvas_capacity) {
    uint8_t * grown = build->allocator->realloc_fn(build->allocator->ctx,
        build->canvas, bytes);

    if (!grown) {
      return gfnt_error_set(error, GFNT_ERR_OOM, build->font.container, 0,
          (uint32_t)build->record_count, "a canvas for a composite glyph");
    }
    build->canvas = grown;
    build->canvas_capacity = bytes;
  }
  // Every time, not only when it grew: the buffer outlives one glyph, and a
  // composite OR-s into it rather than filling it.
  memset(build->canvas, 0, bytes);
  *out_canvas = build->canvas;
  return GFNT_OK;
}

GFNT_Result gfnt_bitmap_build_rows(GFNT_BitmapBuild * build, size_t bytes,
    uint8_t ** out_rows, GFNT_Error * error) {
  if (bytes > build->row_capacity) {
    uint8_t * grown = build->allocator->realloc_fn(build->allocator->ctx,
        build->row, bytes);

    if (!grown) {
      return gfnt_error_set(error, GFNT_ERR_OOM, build->font.container, 0,
          (uint32_t)build->record_count, "a row buffer for a bitmap glyph");
    }
    build->row = grown;
    build->row_capacity = bytes;
  }
  *out_rows = build->row;
  return GFNT_OK;
}

/** One record in a state no glyph data was read for. */
static GFNT_Result gfnt_bitmap_build_placeholder(GFNT_BitmapBuild * build,
    GFNT_BitmapPresence presence, const char * reason, GFNT_Error * error) {
  GFNT_BitmapRecord record;
  void * array = build->records;

  memset(&record, 0, sizeof record);
  record.name = GFNT_BITMAP_NO_STRING;
  record.presence = presence;
  record.reason = reason;

  if (!gfnt_bitmap_reserve(build->allocator, &array, &build->record_capacity,
          build->record_count, sizeof *build->records)) {
    return gfnt_error_set(error, GFNT_ERR_OOM, build->font.container, 0,
        (uint32_t)build->record_count, "the bitmap glyph array");
  }
  build->records = array;
  build->records[build->record_count++] = record;
  return GFNT_OK;
}

GFNT_Result gfnt_bitmap_build_absent(GFNT_BitmapBuild * build,
    GFNT_Error * error) {
  return gfnt_bitmap_build_placeholder(build, GFNT_BITMAP_ABSENT, NULL, error);
}

GFNT_Result gfnt_bitmap_build_corrupt(GFNT_BitmapBuild * build,
    const char * reason, GFNT_Error * error) {
  return gfnt_bitmap_build_placeholder(build, GFNT_BITMAP_CORRUPT, reason,
      error);
}

bool gfnt_bitmap_widen_rows(uint8_t * out, const uint8_t * source,
    size_t available, uint32_t width, uint32_t height) {
  size_t stride = ((size_t)width + 7u) / 8u;
  size_t bits;

  if (!gcu_safe_mul_size(width, height, &bits)) {
    return false;
  }
  if ((bits + 7u) / 8u > available) {
    return false;
  }
  memset(out, 0, stride * (size_t)height);
  for (uint32_t y = 0; y < height; ++y) {
    size_t base = (size_t)y * width;
    uint8_t * row = out + (size_t)y * stride;

    for (uint32_t x = 0; x < width; ++x) {
      size_t at = base + x;

      if ((source[at >> 3] >> (7u - (at & 7u))) & 1u) {
        row[x >> 3] |= (uint8_t)(0x80u >> (x & 7u));
      }
    }
  }
  return true;
}

GFNT_Result gfnt_bitmap_build_map(GFNT_BitmapBuild * build, uint32_t codepoint,
    uint32_t glyph, GFNT_Error * error) {
  void * array = build->map;

  if (!gfnt_bitmap_reserve(build->allocator, &array, &build->map_capacity,
          build->map_count, sizeof *build->map)) {
    return gfnt_error_set(error, GFNT_ERR_OOM, build->font.container, 0,
        glyph, "the bitmap encoding array");
  }
  build->map = array;
  build->map[build->map_count].codepoint = codepoint;
  build->map[build->map_count].glyph = glyph;
  build->map_count += 1;
  return GFNT_OK;
}

/** Order the map by codepoint, and by glyph within one codepoint. */
static int gfnt_bitmap_map_order(const void * left, const void * right) {
  const GFNT_BitmapMapping * a = left;
  const GFNT_BitmapMapping * b = right;

  if (a->codepoint != b->codepoint) {
    return a->codepoint < b->codepoint ? -1 : 1;
  }
  if (a->glyph != b->glyph) {
    return a->glyph < b->glyph ? -1 : 1;
  }
  return 0;
}

void gfnt_bitmap_build_finish(GFNT_BitmapBuild * build,
    GFNT_BitmapFont * out_font, bool keep) {
  const GFNT_Allocator * allocator = build->allocator;

  if (keep && out_font) {
    *out_font = build->font;
    out_font->glyphs = build->records;
    out_font->glyph_count = build->record_count;
    out_font->map = build->map;
    out_font->map_count = build->map_count;
    // Sorted here rather than as it is built: every parser appends in the
    // order its file lists things, and a sort per insertion would be the same
    // answer at n times the cost.
    if (out_font->map_count > 1) {
      qsort(out_font->map, out_font->map_count, sizeof *out_font->map,
          gfnt_bitmap_map_order);
    }
    uint8_t * text = NULL;

    gfnt_buffer_release(&build->pixels, &out_font->pixels,
        &out_font->pixel_length);
    gfnt_buffer_release(&build->text, &text, &out_font->text_length);
    // The arena is characters as far as every reader of it is concerned, and
    // bytes as far as the buffer is; the conversion is here, once, rather than a
    // cast at each use.
    out_font->text = (char *)text;
  }
  else {
    allocator->free_fn(allocator->ctx, build->records);
    allocator->free_fn(allocator->ctx, build->map);
    gfnt_buffer_free(&build->pixels);
    gfnt_buffer_free(&build->text);
  }
  allocator->free_fn(allocator->ctx, build->row);
  allocator->free_fn(allocator->ctx, build->canvas);
  allocator->free_fn(allocator->ctx, build);
}

void gfnt_bitmap_release(const GFNT_Allocator * allocator, void * table) {
  GFNT_BitmapFont * font = table;

  if (!font) {
    return;
  }
  allocator->free_fn(allocator->ctx, font->glyphs);
  allocator->free_fn(allocator->ctx, font->map);
  allocator->free_fn(allocator->ctx, font->pixels);
  allocator->free_fn(allocator->ctx, font->text);
  font->glyphs = NULL;
  font->glyph_count = 0;
  font->map = NULL;
  font->map_count = 0;
  font->pixels = NULL;
  font->text = NULL;
}

// ------------------------------------------------------------ the containers

bool gfnt_bitmap_looks_like(const GFNT_Reader * blob, GFNT_Tag * out_flavour,
    GFNT_Tag * out_tag) {
  // Strongest magic first. PSF 2's is four bytes and PSF 1's two; PCF's is four;
  // BDF's is a keyword at the start of the file; `.hex` has no magic at all and
  // is recognised by its grammar, so it goes last and asks for several lines.
  if (gfnt_psf_looks_like(blob)) {
    *out_flavour = GFNT_FLAVOUR_PSF;
    *out_tag = GFNT_TAG_PSF;
    return true;
  }
  if (gfnt_pcf_looks_like(blob)) {
    *out_flavour = GFNT_FLAVOUR_PCF;
    // PCF's entry tags come from its own table of contents, so the probe's tag
    // is not used for it; naming the container keeps the signature one shape.
    *out_tag = GFNT_FLAVOUR_PCF;
    return true;
  }
  if (gfnt_bdf_looks_like(blob)) {
    *out_flavour = GFNT_FLAVOUR_BDF;
    *out_tag = GFNT_TAG_BDF;
    return true;
  }
  if (gfnt_hex_looks_like(blob)) {
    *out_flavour = GFNT_FLAVOUR_HEX;
    *out_tag = GFNT_TAG_HEX;
    return true;
  }
  return false;
}

/**
 * Parse whichever container the face's flavour says it is.
 *
 * The dispatch is a switch rather than a chain of predicates so that adding a
 * fifth container is a compile error here instead of a silent fall-through, which
 * is the shape `outline/producer.c` settled on for the same reason.
 */
static GFNT_Result gfnt_bitmap_parse(const GFNT_Face * face, void * out,
    void * context,
    GFNT_Error * error) {
  // Zeroed **before** anything can return, because ::gfnt_table_cached() copies
  // its scratch into the memo whatever the result was - so a parser that returns
  // without writing publishes whatever was on the stack. For a memo holding four
  // pointers that the face frees, that is a free() of a wild address rather than a
  // wrong number: this function returned UNSUPPORTED for an outline face without
  // touching `out`, and closing the face segfaulted.
  memset(out, 0, sizeof(GFNT_BitmapFont));
  switch (face->flavour) {
    case GFNT_FLAVOUR_PCF:
      return gfnt_pcf_parse(face, out, context, error);
    case GFNT_FLAVOUR_PSF:
      return gfnt_psf_parse(face, out, context, error);
    case GFNT_FLAVOUR_BDF:
      return gfnt_bdf_parse(face, out, context, error);
    case GFNT_FLAVOUR_HEX:
      return gfnt_hex_parse(face, out, context, error);
    default:
      break;
  }
  return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, face->flavour, 0,
      GFNT_GLYPH_NONE, "not a standalone bitmap container");
}

GFNT_Result gfnt_bitmap_derive(GFNT_Face * face, GFNT_Tag flavour, GFNT_Tag tag,
    GFNT_Error * error) {
  const GFNT_BitmapFont * font = NULL;
  GFNT_Result result;

  if (flavour == GFNT_FLAVOUR_PCF) {
    // PCF is the container design.md section 7.1 wrote the multi-entry directory
    // for: its table of contents is a typed list of extents, so each of its
    // tables gets a directory entry and therefore a reader that spans only
    // itself.
    result = gfnt_pcf_directory(face, error);
  }
  else {
    result = gfnt_sfnt_single_table_directory(face, flavour, tag, error);
  }
  if (result != GFNT_OK) {
    return result;
  }
  // Parsed at load rather than on first use, for the reason a bare CFF is: for a
  // container with no directory the font program *is* the identification, and a
  // face is not handed back for a file whose strike does not parse. The parse's
  // own diagnostic is what the caller gets, not a flattened "not a font".
  return gfnt_face_bitmap(face, &font, error);
}

GFNT_Result gfnt_face_bitmap(const GFNT_Face * face,
    const GFNT_BitmapFont ** out_font, GFNT_Error * error) {
  GFNT_Face * owner = (GFNT_Face *)face;
  GFNT_BitmapFont scratch;
  GFNT_Result result;

  if (!face || !out_font) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_table_cached(face, &owner->bitmap_state, &owner->bitmap,
      &scratch, sizeof scratch, gfnt_bitmap_parse, NULL,
      gfnt_bitmap_release, error);
  if (result == GFNT_OK) {
    *out_font = &owner->bitmap;
  }
  return result;
}

// ------------------------------------------------------------ the accessors

const char * gfnt_bitmap_string(const GFNT_BitmapFont * font, size_t offset) {
  if (!font || offset == GFNT_BITMAP_NO_STRING || offset >= font->text_length) {
    return NULL;
  }
  return font->text + offset;
}

GFNT_Result gfnt_bitmap_glyph_name(const GFNT_BitmapFont * font, uint32_t glyph,
    const char ** out_name) {
  const char * name;

  if (!font || !out_name) {
    return GFNT_ERR_INVALID;
  }
  if (glyph >= font->glyph_count) {
    return GFNT_ERR_INVALID;
  }
  name = gfnt_bitmap_string(font, font->glyphs[glyph].name);
  if (!name) {
    return GFNT_ERR_UNSUPPORTED;
  }
  *out_name = name;
  return GFNT_OK;
}

GFNT_Result gfnt_bitmap_glyph_for_codepoint(const GFNT_BitmapFont * font,
    uint32_t codepoint, uint32_t * out_glyph) {
  size_t low = 0;
  size_t high;

  if (!font || !out_glyph) {
    return GFNT_ERR_INVALID;
  }
  if (!font->states_encoding) {
    return GFNT_ERR_UNSUPPORTED;
  }
  high = font->map_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;

    if (font->map[middle].codepoint < codepoint) {
      low = middle + 1;
    }
    else {
      high = middle;
    }
  }
  if (low >= font->map_count || font->map[low].codepoint != codepoint) {
    return GFNT_ERR_INVALID;
  }
  *out_glyph = font->map[low].glyph;
  return GFNT_OK;
}

/**
 * Turn a record into the public glyph.
 *
 * Both paths end here - a standalone container's one strike and an `EBLC`
 * strike's - because they differ in how a record is *found* and not at all in what
 * a record means. Two copies of this would be two chances for `bits` to be wrong
 * for an empty glyph.
 */
static GFNT_Result gfnt_bitmap_answer(const GFNT_BitmapFont * font,
    uint32_t glyph, GFNT_BitmapGlyph * out_glyph) {
  const GFNT_BitmapRecord * record = &font->glyphs[glyph];

  *out_glyph = (GFNT_BitmapGlyph) {
    .width = record->width,
    .height = record->height,
    .stride = record->stride,
    .bearing_x = record->bearing_x,
    .bearing_y = record->bearing_y,
    .advance = record->advance,
    .bit_depth = font->strike.bit_depth,
    .bits = record->height ? font->pixels + record->offset : NULL,
    .strike = font->strike,
  };
  return GFNT_OK;
}

GFNT_Result gfnt_face_glyph_bitmap(const GFNT_Face * face, uint32_t glyph,
    size_t strike, GFNT_BitmapGlyph * out_glyph, GFNT_Error * error) {
  const GFNT_BitmapFont * font = NULL;
  GFNT_Result result;

  gfnt_error_clear(error);
  if (!face || !out_glyph) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no face, or nowhere to put the glyph");
  }
  result = gfnt_face_bitmap(face, &font, error);
  if (result != GFNT_OK) {
    if (result == GFNT_ERR_UNSUPPORTED) {
      // Being asked for pixels from a face that has none is not the same as
      // being asked for pixels this library cannot reach, and glyph.h's strike
      // count draws the same distinction for the same reason.
      size_t count = 0;
      GFNT_Error strikes;
      GFNT_Result counted;

      gfnt_error_clear(&strikes);
      counted = gfnt_face_strike_count(face, &count, &strikes);
      if (counted != GFNT_OK) {
        if (error) {
          *error = strikes;
        }
        // **The result, not a constant.** This returned GFNT_ERR_UNSUPPORTED
        // whatever had gone wrong, which was right while the only failure a strike
        // count could have was "this library cannot enumerate them". An EBLC parse
        // can also run out of memory or exceed a limit, and that arrived here as
        // UNSUPPORTED carrying an out-of-memory message - a code and a diagnostic
        // disagreeing, which is worse than either being wrong alone.
        return counted;
      }
      if (count > 0) {
        // An EBLC face. The strike is parsed whole on first use and each strike
        // separately, so asking about one glyph of one size does not read the
        // other five strikes a font like uming.ttc carries.
        if (strike >= count) {
          return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_EBLC, 0, glyph,
              "a strike index the face does not have");
        }
        result = gfnt_eblc_strike_glyphs(face, strike, &font, error);
        if (result != GFNT_OK) {
          return result;
        }
        if (glyph >= font->glyph_count) {
          return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_EBDT, 0, glyph,
              "a glyph index past the end of the face");
        }
        if (font->glyphs[glyph].presence == GFNT_BITMAP_ABSENT) {
          // A strike is sparse over the face: this one does not carry the glyph.
          // UNSUPPORTED rather than INVALID, because the glyph index is perfectly
          // good and another strike may well answer for it.
          return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_EBDT, 0,
              glyph, "a glyph this strike does not carry");
        }
        if (font->glyphs[glyph].presence == GFNT_BITMAP_CORRUPT) {
          // M11: the strike claims the glyph and its bytes do not make sense, and
          // that condemns this glyph rather than the strike. Every other glyph of
          // the strike still answers, which is the whole point.
          return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBDT, 0, glyph,
              font->glyphs[glyph].reason
                  ? font->glyphs[glyph].reason
                  : "a glyph whose EBDT data this strike gets wrong");
        }
        return gfnt_bitmap_answer(font, glyph, out_glyph);
      }
      return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, face->flavour, 0,
          glyph, "a face with no bitmap strikes at all");
    }
    return result;
  }
  if (strike != 0) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, font->container, 0, glyph,
        "a standalone bitmap font has exactly one strike, which is strike 0");
  }
  if (glyph >= font->glyph_count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, font->container, 0, glyph,
        "a glyph index past the end of the font");
  }
  return gfnt_bitmap_answer(font, glyph, out_glyph);
}

uint8_t gfnt_bitmap_pixel(const GFNT_BitmapGlyph * glyph, uint32_t x,
    uint32_t y) {
  size_t at;

  if (!glyph || !glyph->bits || x >= glyph->width || y >= glyph->height) {
    return 0;
  }
  at = (size_t)y * glyph->stride + x / 8u;
  return (glyph->bits[at] & (uint8_t)(0x80u >> (x % 8u))) ? 255u : 0u;
}

GFNT_Result gfnt_bitmap_dump(const GFNT_BitmapGlyph * glyph, FILE * out) {
  if (!glyph || !out) {
    return GFNT_ERR_INVALID;
  }
  if (fprintf(out,
          "bitmap: %ux%u, bearing %d,%d, advance %d, %u bpp, strike %ux%u\n",
          (unsigned)glyph->width, (unsigned)glyph->height,
          (int)glyph->bearing_x, (int)glyph->bearing_y, (int)glyph->advance,
          (unsigned)glyph->bit_depth, (unsigned)glyph->strike.ppem_x,
          (unsigned)glyph->strike.ppem_y)
      < 0) {
    return GFNT_ERR_IO;
  }
  for (uint32_t y = 0; y < glyph->height; ++y) {
    for (uint32_t x = 0; x < glyph->width; ++x) {
      if (fputc(gfnt_bitmap_pixel(glyph, x, y) ? '#' : '.', out) == EOF) {
        return GFNT_ERR_IO;
      }
    }
    if (fputc('\n', out) == EOF) {
      return GFNT_ERR_IO;
    }
  }
  return GFNT_OK;
}

GFNT_Result gfnt_face_bitmap_encoding_count(const GFNT_Face * face,
    size_t * out_count, GFNT_Error * error) {
  const GFNT_BitmapFont * font = NULL;
  GFNT_Result result;

  gfnt_error_clear(error);
  if (!face || !out_count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no face, or nowhere to put the count");
  }
  result = gfnt_face_bitmap(face, &font, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (!font->states_encoding) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, font->container, 0,
        GFNT_GLYPH_NONE,
        "a bitmap font that states no mapping from characters to glyphs");
  }
  *out_count = font->map_count;
  return GFNT_OK;
}

GFNT_Result gfnt_face_bitmap_encoding_at(const GFNT_Face * face, size_t index,
    uint32_t * out_codepoint, uint32_t * out_glyph, GFNT_Error * error) {
  const GFNT_BitmapFont * font = NULL;
  size_t count = 0;
  GFNT_Result result;

  result = gfnt_face_bitmap_encoding_count(face, &count, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (index >= count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no such entry in the font's encoding");
  }
  result = gfnt_face_bitmap(face, &font, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (out_codepoint) {
    *out_codepoint = font->map[index].codepoint;
  }
  if (out_glyph) {
    *out_glyph = font->map[index].glyph;
  }
  return GFNT_OK;
}

bool gfnt_face_is_bitmap(const GFNT_Face * face) {
  if (!face) {
    return false;
  }
  // The flavour rather than the memo, because every caller of this asks it in
  // order to decide whether to *start* a bitmap parse - a predicate that parsed
  // the font to answer "is this a bitmap font" would charge every metric lookup
  // for the whole strike.
  return face->flavour == GFNT_FLAVOUR_PCF || face->flavour == GFNT_FLAVOUR_BDF
      || face->flavour == GFNT_FLAVOUR_PSF || face->flavour == GFNT_FLAVOUR_HEX;
}

bool gfnt_bitmap_glyph_bound(const GFNT_Face * face, size_t * out_bound) {
  const GFNT_BitmapFont * font = NULL;

  if (!gfnt_face_is_bitmap(face)) {
    return false;
  }
  if (gfnt_face_bitmap(face, &font, NULL) != GFNT_OK) {
    return false;
  }
  *out_bound = font->glyph_count;
  return true;
}

GFNT_Result gfnt_bitmap_name_at(const GFNT_Face * face, uint32_t glyph,
    char * out, size_t capacity, size_t * out_length, GFNT_Error * error) {
  const GFNT_BitmapFont * font = NULL;
  const char * name = NULL;
  size_t length;
  GFNT_Result result;

  if (!out_length) {
    return GFNT_ERR_INVALID;
  }
  result = gfnt_face_bitmap(face, &font, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (glyph >= font->glyph_count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, font->container, 0, glyph,
        "this glyph index is past the face's glyph count");
  }
  result = gfnt_bitmap_glyph_name(font, glyph, &name);
  if (result == GFNT_ERR_UNSUPPORTED) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, font->container, 0, glyph,
        "a bitmap font that states no glyph names - PSF and .hex state none at "
        "all, and a PCF may ship without the table");
  }
  if (result != GFNT_OK) {
    return result;
  }
  length = strlen(name);
  *out_length = length;
  if (!out) {
    return GFNT_OK;
  }
  if (length + 1 > capacity) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, font->container, 0, glyph,
        "a glyph name longer than the caller's buffer");
  }
  memcpy(out, name, length + 1);
  return GFNT_OK;
}

GFNT_Result gfnt_bitmap_string_for(const GFNT_Face * face,
    GFNT_BitmapString which, const char ** out_text, GFNT_Error * error) {
  const GFNT_BitmapFont * font = NULL;
  size_t offset = GFNT_BITMAP_NO_STRING;
  const char * text;
  GFNT_Result result = gfnt_face_bitmap(face, &font, error);

  if (result != GFNT_OK) {
    return result;
  }
  switch (which) {
    case GFNT_BITMAP_STRING_FAMILY:
      offset = font->family;
      break;
    case GFNT_BITMAP_STRING_FULL:
      offset = font->full_name;
      break;
    case GFNT_BITMAP_STRING_COPYRIGHT:
      offset = font->copyright;
      break;
    case GFNT_BITMAP_STRING_WEIGHT:
      offset = font->weight;
      break;
  }
  text = gfnt_bitmap_string(font, offset);
  if (!text) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, font->container, 0,
        GFNT_GLYPH_NONE, "a bitmap font that states no such name");
  }
  *out_text = text;
  return GFNT_OK;
}
