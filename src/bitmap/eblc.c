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
 * The `EBLC` table: one `bitmapSizeTable` per strike.
 *
 * documentation/design.md section 7.5. `eblc.h` says why this is the list and
 * not the pixels.
 *
 * Reference: OpenType Specification 1.9, "EBLC - Embedded Bitmap Location
 * Table".
 */

#include <ghoti.io/font/macros.h>
#include <inttypes.h>
#include <string.h>
#include "../tables/tables.h"
#include "eblc.h"

/** The one version the specification defines, and the only one in the wild. */
#define GFNT_EBLC_VERSION 0x00020000u

/** Bytes per `bitmapSizeTable`. */
#define GFNT_EBLC_SIZE_TABLE_BYTES 48u

/** Bytes in the `EBLC` header before the first `bitmapSizeTable`. */
#define GFNT_EBLC_HEADER_BYTES 8u

/** `flags` bit 0: the strike states horizontal line metrics. */
#define GFNT_EBLC_HORIZONTAL 0x01
/** `flags` bit 1: the strike states vertical line metrics. */
#define GFNT_EBLC_VERTICAL 0x02

/**
 * Read one `sbitLineMetrics`.
 *
 * Twelve bytes, the last two of which the specification reserves and no file
 * uses. They are skipped rather than stored: a reserved byte nothing reads is
 * the `unread-table-constants` shape, and a dump that printed them would invite
 * a differential to compare them.
 */
static GFNT_Result gfnt_eblc_read_line_metrics(GFNT_Reader * reader,
    GFNT_EblcLineMetrics * out) {
  if (gfnt_read_s8(reader, &out->ascender) != GFNT_OK
      || gfnt_read_s8(reader, &out->descender) != GFNT_OK
      || gfnt_read_u8(reader, &out->width_max) != GFNT_OK
      || gfnt_read_s8(reader, &out->caret_slope_numerator) != GFNT_OK
      || gfnt_read_s8(reader, &out->caret_slope_denominator) != GFNT_OK
      || gfnt_read_s8(reader, &out->caret_offset) != GFNT_OK
      || gfnt_read_s8(reader, &out->min_origin_sb) != GFNT_OK
      || gfnt_read_s8(reader, &out->min_advance_sb) != GFNT_OK
      || gfnt_read_s8(reader, &out->max_before_bl) != GFNT_OK
      || gfnt_read_s8(reader, &out->min_after_bl) != GFNT_OK
      || gfnt_reader_skip(reader, 2) != GFNT_OK) {
    return GFNT_ERR_CORRUPT;
  }
  return GFNT_OK;
}

/**
 * What a glyph from a strike of this depth comes back as.
 *
 * 32 is the colour depth and belongs to `CBLC`, which this file does not read,
 * so it has no arm here: a depth this function cannot name is refused by the
 * caller rather than mapped to a plausible kind.
 */
static bool gfnt_eblc_kind_for_depth(uint8_t depth, GFNT_GlyphKind * out_kind) {
  switch (depth) {
    case 1:
      *out_kind = GFNT_GLYPH_BITMAP_MONO;
      return true;
    case 2:
    case 4:
    case 8:
      *out_kind = GFNT_GLYPH_BITMAP_GRAY;
      return true;
    default:
      return false;
  }
}

/** Parse one `bitmapSizeTable` at the reader's cursor. */
static GFNT_Result gfnt_eblc_read_strike(const GFNT_Face * face,
    GFNT_Reader * reader, size_t index, size_t table_length,
    GFNT_EblcStrike * out, GFNT_Error * error) {
  size_t offset = gfnt_reader_tell(reader);
  uint8_t ppem_x = 0;
  uint8_t ppem_y = 0;
  GFNT_GlyphKind kind = GFNT_GLYPH_NONE;

  (void)face;
  if (gfnt_read_u32(reader, &out->index_array_offset) != GFNT_OK
      || gfnt_read_u32(reader, &out->index_tables_size) != GFNT_OK
      || gfnt_read_u32(reader, &out->index_subtable_count) != GFNT_OK
      || gfnt_read_u32(reader, &out->colour_ref) != GFNT_OK
      || gfnt_eblc_read_line_metrics(reader, &out->horizontal) != GFNT_OK
      || gfnt_eblc_read_line_metrics(reader, &out->vertical) != GFNT_OK
      || gfnt_read_u16(reader, &out->start_glyph) != GFNT_OK
      || gfnt_read_u16(reader, &out->end_glyph) != GFNT_OK
      || gfnt_read_u8(reader, &ppem_x) != GFNT_OK
      || gfnt_read_u8(reader, &ppem_y) != GFNT_OK
      || gfnt_read_u8(reader, &out->bit_depth) != GFNT_OK
      || gfnt_read_s8(reader, &out->flags) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBLC, offset,
        GFNT_GLYPH_NONE, "an EBLC bitmapSizeTable ends past the table");
  }

  if (ppem_x == 0 || ppem_y == 0) {
    // A strike drawn at no pixels per em describes nothing, and it would sit in
    // the list as a candidate every nearest-strike search had to consider.
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBLC, offset,
        GFNT_GLYPH_NONE, "an EBLC strike states a pixel size of zero");
  }
  if (!gfnt_eblc_kind_for_depth(out->bit_depth, &kind)) {
    // 32 reaches here as well as 3 or 7: a colour strike is CBLC's, and this
    // table claiming one is not a depth this library can answer for.
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBLC, offset,
        GFNT_GLYPH_NONE, "an EBLC strike states a bit depth that is not 1, 2, "
        "4 or 8");
  }
  if (out->end_glyph < out->start_glyph) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBLC, offset,
        GFNT_GLYPH_NONE, "an EBLC strike's glyph range runs backwards");
  }
  if ((out->flags & (GFNT_EBLC_HORIZONTAL | GFNT_EBLC_VERTICAL)) == 0) {
    // The ascent and descent a caller reads have to come from one of the two
    // sbitLineMetrics, and a strike that claims neither direction has not said
    // which. Answering "horizontal" would be inventing the fact.
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBLC, offset,
        GFNT_GLYPH_NONE, "an EBLC strike states neither horizontal nor "
        "vertical line metrics");
  }
  // Where the index lives is checked, how long it claims to be is not. mona.ttf
  // - one of the two files in Debian with this table - states an indexTablesSize
  // that overlaps its neighbour's region by eight bytes, and fontTools refuses
  // the font for it while every renderer displays it, because no renderer reads
  // the field. So the offset is validated here and the extent is bounded by the
  // file and by what each subtable's own format needs, where the index is read.
  if (out->index_array_offset >= table_length) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBLC, offset,
        GFNT_GLYPH_NONE,
        "an EBLC strike's indexSubTableArrayOffset is past the table");
  }

  out->strike = (GFNT_Strike) {
    .index = index,
    .ppem_x = ppem_x,
    .ppem_y = ppem_y,
    .bit_depth = out->bit_depth,
    .kind = kind,
    // The direction the strike says it is for decides which metrics are its
    // own; a strike stating both is horizontal, which is what a horizontal
    // layout engine asks for and what every file in the population states.
    .ascent = (out->flags & GFNT_EBLC_HORIZONTAL) ? out->horizontal.ascender
                                                  : out->vertical.ascender,
    .descent = (out->flags & GFNT_EBLC_HORIZONTAL) ? out->horizontal.descender
                                                   : out->vertical.descender,
  };
  return GFNT_OK;
}

GFNT_Result gfnt_eblc_parse(const GFNT_Face * face, void * out,
    GFNT_Error * error) {
  GFNT_Eblc * eblc = out;
  GFNT_Reader reader;
  GFNT_Result result;
  uint32_t count = 0;
  size_t length;

  // gfnt_table_cached() copies this scratch into the memo whatever comes back,
  // so every failure below has to leave something the face can free (tables.h).
  memset(eblc, 0, sizeof *eblc);

  result = gfnt_face_table_reader(face, GFNT_TAG_EBLC, &reader, error);
  if (result != GFNT_OK) {
    return result;
  }
  length = gfnt_reader_remaining(&reader);

  if (gfnt_read_u32(&reader, &eblc->version) != GFNT_OK
      || gfnt_read_u32(&reader, &count) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBLC, 0,
        GFNT_GLYPH_NONE, "an EBLC shorter than its own header");
  }
  if (eblc->version != GFNT_EBLC_VERSION) {
    // A version this library has not been written against describes strikes it
    // would be guessing at, which is UNSUPPORTED rather than corrupt.
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_EBLC, 0,
        GFNT_GLYPH_NONE, "an EBLC version this library does not read");
  }
  if (count > face->limits.max_strikes) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, GFNT_TAG_EBLC, 4,
        GFNT_GLYPH_NONE, "more EBLC strikes than GFNT_Limits::max_strikes");
  }
  if (count == 0) {
    // An EBLC that lists no strikes. The table is well formed and says the font
    // has no bitmaps, which is a different answer from having no table at all
    // only in that the directory carries one - and nothing downstream needs the
    // difference, so it is reported as zero strikes rather than refused.
    return GFNT_OK;
  }
  // There is deliberately **no bulk check** that count * 48 bytes are present,
  // and the first draft had one. It refused exactly the inputs the per-strike
  // read below refuses, which made that read's failure arm unreachable - two
  // guards answering one question, where the second can never be seen to work.
  // What bounds the allocation is max_strikes above, which is the cap that
  // matters; a count with no tables behind it costs one array and is then
  // refused at the first field that runs off the end.

  eblc->strikes = face->allocator->calloc_fn(face->allocator->ctx, count,
      sizeof *eblc->strikes);
  if (!eblc->strikes) {
    return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_EBLC, 0,
        GFNT_GLYPH_NONE, "no memory for the EBLC strike list");
  }
  eblc->strike_count = count;

  for (uint32_t i = 0; i < count; ++i) {
    result = gfnt_eblc_read_strike(face, &reader, i, length,
        &eblc->strikes[i], error);
    if (result != GFNT_OK) {
      return result;
    }
  }
  return GFNT_OK;
}

void gfnt_eblc_release(const GFNT_Allocator * allocator, void * table) {
  GFNT_Eblc * eblc = table;

  if (!allocator || !eblc) {
    return;
  }
  allocator->free_fn(allocator->ctx, eblc->strikes);
  eblc->strikes = NULL;
  eblc->strike_count = 0;
}

bool gfnt_face_has_eblc(const GFNT_Face * face) {
  if (!face) {
    return false;
  }
  // Both tables, because neither names a glyph without the other - the rule
  // gfnt_sfnt_producer() applies to glyf and loca, for the same reason.
  return gfnt_face_has_table(face, GFNT_TAG_EBLC)
      && gfnt_face_has_table(face, GFNT_TAG_EBDT);
}

GFNT_Result gfnt_face_eblc(const GFNT_Face * face, const GFNT_Eblc ** out_eblc,
    GFNT_Error * error) {
  GFNT_Eblc scratch;
  GFNT_Result result;

  if (!face || !out_eblc) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no face, or nowhere to put the table");
  }
  if (!gfnt_face_has_eblc(face)) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, 0, 0, GFNT_GLYPH_NONE,
        "the face has no EBLC with an EBDT to go with it");
  }
  result = gfnt_table_cached(face, &((GFNT_Face *)face)->eblc_state,
      &((GFNT_Face *)face)->eblc, &scratch, sizeof scratch, gfnt_eblc_parse,
      gfnt_eblc_release, error);
  if (result != GFNT_OK) {
    return result;
  }
  *out_eblc = &face->eblc;
  return GFNT_OK;
}

GFNT_Result gfnt_eblc_dump(const GFNT_Eblc * eblc, FILE * out) {
  if (!eblc || !out) {
    return GFNT_ERR_INVALID;
  }
  if (fprintf(out, "EBLC: version 0x%08" PRIx32 ", %zu strikes\n",
          eblc->version, eblc->strike_count)
      < 0) {
    return GFNT_ERR_IO;
  }
  for (size_t i = 0; i < eblc->strike_count; ++i) {
    const GFNT_EblcStrike * strike = &eblc->strikes[i];

    if (fprintf(out,
            "EBLC: strike %zu, ppem %" PRIu32 "x%" PRIu32 ", depth %u, "
            "flags 0x%02x, glyphs %u..%u, ascent %" PRId32 ", descent %"
            PRId32 ", %" PRIu32 " index subtables\n",
            i, strike->strike.ppem_x, strike->strike.ppem_y,
            (unsigned)strike->bit_depth, (unsigned)(strike->flags & 0xFF),
            (unsigned)strike->start_glyph, (unsigned)strike->end_glyph,
            strike->strike.ascent, strike->strike.descent,
            strike->index_subtable_count)
        < 0) {
      return GFNT_ERR_IO;
    }
  }
  return GFNT_OK;
}
