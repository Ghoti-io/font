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
#include <ghoti.io/cutil/safemath.h>
#include <string.h>
#include "../tables/tables.h"
#include "eblc.h"

/** Bytes per `bitmapSizeTable`. */
#define GFNT_EBLC_SIZE_TABLE_BYTES 48u

GFNT_Result gfnt_eblc_read_line_metrics(GFNT_Reader * reader,
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

int8_t gfnt_eblc_descent_of(const GFNT_EblcLineMetrics * metrics) {
  // The specification does not say which sign `descender` carries, and the
  // population is split: 30 faces write it negative, 4 write the magnitude, and 2
  // write zero. `minAfterBL` is the same quantity measured a second way in the
  // same record - "largest ink extent below the baseline" - so where the two
  // disagree about the direction and `minAfterBL` has committed to one, that is
  // the statement to follow.
  //
  // Anonymous Pro is every one of the four: `descender` 2 and `minAfterBL` -2 at
  // every strike. The font means two pixels below the baseline and wrote the
  // magnitude, and this library reported `descent` of +2 - against its own
  // documented convention, so a caller laying out a line from it put the
  // descenders *above* the baseline. ::GFNT_Strike said "negative" and did not
  // enforce it, which is the shape where a contract and its code can differ
  // without anything failing.
  //
  // FreeType does the same comparison in `tt_face_load_strike_metrics` for the
  // same reason, and comments it "fuzzy wording in the EBLC documentation".
  //
  // The narrowness matters. A positive `descender` with a `minAfterBL` that is
  // **not** negative is left alone: there is no second statement to prefer, so
  // flipping it would be inventing a direction rather than resolving a conflict.
  if (metrics->descender > 0 && metrics->min_after_bl < 0) {
    return (int8_t)-metrics->descender;
  }
  return metrics->descender;
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
    .descent = gfnt_eblc_descent_of((out->flags & GFNT_EBLC_HORIZONTAL)
        ? &out->horizontal : &out->vertical),
  };
  return GFNT_OK;
}

GFNT_Result gfnt_eblc_parse(const GFNT_Face * face, void * out,
    void * context,
    GFNT_Error * error) {
  // There is one of this table per face, so there is nothing to select; the
  // parameter is here because every memo parser shares one signature.
  (void)context;
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
  eblc->strike_states = face->allocator->calloc_fn(face->allocator->ctx, count,
      sizeof *eblc->strike_states);
  eblc->strike_glyphs = face->allocator->calloc_fn(face->allocator->ctx, count,
      sizeof *eblc->strike_glyphs);
  if (!eblc->strikes || !eblc->strike_states || !eblc->strike_glyphs) {
    // strike_count stays zero until all three are there, so a release of this
    // half-built struct frees the arrays and walks no glyph memo.
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
  // Each strike's glyphs are a bitmap font of their own, with arenas of their
  // own, parsed only if something asked for that strike - so most of these are
  // zeroed structs and the release of one is a no-op.
  for (size_t i = 0; i < eblc->strike_count; ++i) {
    gfnt_bitmap_release(allocator, &eblc->strike_glyphs[i]);
  }
  allocator->free_fn(allocator->ctx, eblc->strike_glyphs);
  allocator->free_fn(allocator->ctx, eblc->strike_states);
  allocator->free_fn(allocator->ctx, eblc->strikes);
  eblc->strikes = NULL;
  eblc->strike_states = NULL;
  eblc->strike_glyphs = NULL;
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
      &((GFNT_Face *)face)->eblc, &scratch, sizeof scratch, gfnt_eblc_parse, NULL,
      gfnt_eblc_release, error);
  if (result != GFNT_OK) {
    return result;
  }
  *out_eblc = &face->eblc;
  return GFNT_OK;
}

/** Bytes in an `indexSubTableArray` entry: two glyph ids and an offset. */
#define GFNT_EBLC_ARRAY_ENTRY_BYTES 8u

/** Bytes in an `indexSubHeader`: two formats and an `EBDT` offset. */
#define GFNT_EBLC_SUBHEADER_BYTES 8u

/** Bytes in a `BigGlyphMetrics`. `SmallGlyphMetrics` is its first five. */
#define GFNT_EBLC_BIG_METRICS_BYTES 8u

/**
 * Read a `BigGlyphMetrics`, keeping the horizontal five.
 *
 * The three vertical bytes are skipped rather than stored, for the reason
 * ::GFNT_EblcMetrics gives: nothing reads a vertical advance, and a field parsed
 * but unread is one a differential can compare without either side meaning it.
 */
static GFNT_Result gfnt_eblc_read_big_metrics(GFNT_Reader * reader,
    GFNT_EblcMetrics * out) {
  if (gfnt_read_u8(reader, &out->height) != GFNT_OK
      || gfnt_read_u8(reader, &out->width) != GFNT_OK
      || gfnt_read_s8(reader, &out->bearing_x) != GFNT_OK
      || gfnt_read_s8(reader, &out->bearing_y) != GFNT_OK
      || gfnt_read_u8(reader, &out->advance) != GFNT_OK
      || gfnt_reader_skip(reader, 3) != GFNT_OK) {
    return GFNT_ERR_CORRUPT;
  }
  return GFNT_OK;
}

/** Whether this library reads glyph data in this image format. */
static bool gfnt_ebdt_format_known(uint16_t format) {
  switch (format) {
    case 1: // small metrics, byte-aligned
    case 2: // small metrics, bit-aligned
    case 5: // metrics in EBLC, bit-aligned
    case 6: // big metrics, byte-aligned
    case 7: // big metrics, bit-aligned
    case 8: // small metrics and a pad byte, then components
    case 9: // big metrics, then components
      return true;
    default:
      // 3 and 4 the specification itself calls obsolete - 4 is a compressed form
      // nothing has written for decades. Neither is corrupt, and a font with one
      // is declined rather than misread.
      return false;
  }
}

/**
 * Whether an image format takes its metrics from the index rather than the data.
 *
 * Format 5 only, and it is the **reason index formats 2 and 5 exist**: they state
 * one `BigGlyphMetrics` for every glyph they cover, so the glyph data is nothing
 * but rows. A format 5 subtable indexed by any other format states metrics
 * nowhere, which is a font that cannot be read rather than one that is corrupt.
 */
static bool gfnt_ebdt_metrics_from_index(uint16_t image_format) {
  return image_format == 5;
}

GFNT_Result gfnt_eblc_strike_index(const GFNT_Face * face,
    const GFNT_EblcStrike * strike, GFNT_EblcSubtable ** out_subtables,
    size_t * out_count, GFNT_Error * error) {
  GFNT_Reader table;
  GFNT_EblcSubtable * subtables = NULL;
  GFNT_Result result;
  size_t count = strike->index_subtable_count;

  *out_subtables = NULL;
  *out_count = 0;

  result = gfnt_face_table_reader(face, GFNT_TAG_EBLC, &table, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (count == 0) {
    // A strike that lists no index subtables carries no glyphs. Well formed, and
    // the glyph parse below simply marks every glyph absent.
    return GFNT_OK;
  }
  // The same ceiling the glyph count gets: a subtable per glyph is the most a
  // well-formed strike can need, and a strike claiming more is claiming subtables
  // that cannot each cover a glyph.
  if (count > face->limits.max_glyphs) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, GFNT_TAG_EBLC, 0,
        GFNT_GLYPH_NONE,
        "more EBLC index subtables than GFNT_Limits::max_glyphs");
  }

  subtables = face->allocator->calloc_fn(face->allocator->ctx, count,
      sizeof *subtables);
  if (!subtables) {
    return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_EBLC, 0,
        GFNT_GLYPH_NONE, "no memory for an EBLC strike's index");
  }

  for (size_t i = 0; i < count; ++i) {
    GFNT_EblcSubtable * entry = &subtables[i];
    GFNT_Reader at = table;
    uint32_t additional = 0;
    size_t array_at;

    // indexSubTableArrayOffset is from the start of EBLC, and
    // additionalOffsetToIndexSubtable is from the start of *that array* - not from
    // EBLC and not from the entry. Two bases, one of which is itself an offset.
    if (!gcu_safe_mul_size(i, GFNT_EBLC_ARRAY_ENTRY_BYTES, &array_at)
        || !gcu_safe_add_size(array_at, strike->index_array_offset, &array_at)) {
      result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBLC, 0,
          GFNT_GLYPH_NONE, "an EBLC index array that does not fit a size_t");
      goto failed;
    }
    if (gfnt_reader_seek(&at, array_at) != GFNT_OK
        || gfnt_read_u16(&at, &entry->first_glyph) != GFNT_OK
        || gfnt_read_u16(&at, &entry->last_glyph) != GFNT_OK
        || gfnt_read_u32(&at, &additional) != GFNT_OK) {
      result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBLC, array_at,
          GFNT_GLYPH_NONE,
          "an EBLC indexSubTableArray entry past the end of the table");
      goto failed;
    }
    if (entry->last_glyph < entry->first_glyph) {
      result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBLC, array_at,
          GFNT_GLYPH_NONE,
          "an EBLC index subtable whose glyph range runs backwards");
      goto failed;
    }

    if (!gcu_safe_add_size(strike->index_array_offset, additional,
            &entry->body_offset)) {
      result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBLC, array_at,
          GFNT_GLYPH_NONE, "an EBLC subtable offset that does not fit a size_t");
      goto failed;
    }
    if (gfnt_reader_seek(&at, entry->body_offset) != GFNT_OK
        || gfnt_read_u16(&at, &entry->index_format) != GFNT_OK
        || gfnt_read_u16(&at, &entry->image_format) != GFNT_OK
        || gfnt_read_u32(&at, &entry->image_data_offset) != GFNT_OK) {
      result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBLC,
          entry->body_offset, GFNT_GLYPH_NONE,
          "an EBLC indexSubHeader past the end of the table");
      goto failed;
    }
    entry->body_offset += GFNT_EBLC_SUBHEADER_BYTES;

    if (entry->index_format < 1 || entry->index_format > 5) {
      result = gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_EBLC,
          entry->body_offset, GFNT_GLYPH_NONE,
          "an EBLC index subtable format the specification does not define");
      goto failed;
    }
    if (!gfnt_ebdt_format_known(entry->image_format)) {
      result = gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_EBDT,
          entry->body_offset, GFNT_GLYPH_NONE,
          "an EBDT image format this library does not read - 3 and 4, which the "
          "specification itself calls obsolete");
      goto failed;
    }
    if (gfnt_ebdt_metrics_from_index(entry->image_format)
        && !gfnt_eblc_index_is_constant(entry->index_format)) {
      // Image format 5 states no metrics of its own, so the index has to - and
      // only formats 2 and 5 do. This pairing is the one cross-check between the
      // two tables that a font can fail while both halves look well formed on
      // their own.
      result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBLC,
          entry->body_offset, GFNT_GLYPH_NONE,
          "an EBDT image format 5, whose metrics are the index's, under an index "
          "format that states none");
      goto failed;
    }

    if (gfnt_eblc_index_is_constant(entry->index_format)) {
      if (gfnt_read_u32(&at, &entry->image_size) != GFNT_OK
          || gfnt_eblc_read_big_metrics(&at, &entry->metrics) != GFNT_OK) {
        result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBLC,
            entry->body_offset, GFNT_GLYPH_NONE,
            "an EBLC index subtable too short for its constant metrics");
        goto failed;
      }
    }
    if (entry->index_format == 4 || entry->index_format == 5) {
      if (gfnt_read_u32(&at, &entry->sparse_count) != GFNT_OK) {
        result = gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBLC,
            entry->body_offset, GFNT_GLYPH_NONE,
            "an EBLC sparse index subtable with no glyph count");
        goto failed;
      }
      if (entry->sparse_count > face->limits.max_glyphs) {
        result = gfnt_error_set(error, GFNT_ERR_LIMIT, GFNT_TAG_EBLC,
            entry->body_offset, GFNT_GLYPH_NONE,
            "an EBLC index subtable listing more glyphs than "
            "GFNT_Limits::max_glyphs");
        goto failed;
      }
    }
  }

  *out_subtables = subtables;
  *out_count = count;
  return GFNT_OK;

failed:
  face->allocator->free_fn(face->allocator->ctx, subtables);
  return result;
}

/** The context ::gfnt_eblc_strike_parse() is handed: which strike to read. */
typedef struct GFNT_EblcStrikeRequest {
  size_t index;
} GFNT_EblcStrikeRequest;

static GFNT_Result gfnt_eblc_strike_parse(const GFNT_Face * face, void * out,
    void * context, GFNT_Error * error) {
  const GFNT_EblcStrikeRequest * request = context;
  GFNT_BitmapFont * font = out;
  const GFNT_Eblc * eblc = NULL;
  const GFNT_EblcStrike * strike;
  GFNT_EblcSubtable * subtables = NULL;
  size_t subtable_count = 0;
  GFNT_BitmapBuild * build = NULL;
  GFNT_Result result;
  size_t glyphs = 0;

  // Zeroed before anything can return: gfnt_table_cached() copies this scratch
  // into the memo whatever comes back, and the memo is what the face frees.
  memset(font, 0, sizeof *font);

  result = gfnt_face_eblc(face, &eblc, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (request->index >= eblc->strike_count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_EBLC, 0,
        GFNT_GLYPH_NONE, "a strike index the face does not have");
  }
  strike = &eblc->strikes[request->index];

  if (strike->bit_depth != 1) {
    // Checked here rather than in ebdt.c, and before anything else, so that a grey
    // strike costs nothing: not the glyph count, not the index, not an arena.
    //
    // The strike *list* reports such a strike honestly - its depth and
    // ::GFNT_GLYPH_BITMAP_GRAY - because that is what the table says it is. Its
    // glyph data is another matter: every row in this library is one bit per
    // pixel, from ::GFNT_BitmapRecord::stride through gfnt_bitmap_widen_rows() to
    // gfnt_coverage_from_bitmap(), so two or four bits per pixel is a change to
    // all of them rather than an arm in the reader. Refused by name, so that a
    // caller is not handed a glyph whose pixels mean something other than
    // coverage.
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_EBDT, 0,
        GFNT_GLYPH_NONE,
        "a grey EBDT strike, whose glyph data this library does not unpack - "
        "every row it handles is one bit per pixel");
  }

  result = gfnt_face_num_glyphs(face, &glyphs, error);
  if (result != GFNT_OK) {
    return result;
  }

  result = gfnt_eblc_strike_index(face, strike, &subtables, &subtable_count,
      error);
  if (result != GFNT_OK) {
    return result;
  }

  result = gfnt_bitmap_build_start(&build, face, GFNT_TAG_EBDT, glyphs, error);
  if (result != GFNT_OK) {
    face->allocator->free_fn(face->allocator->ctx, subtables);
    return result;
  }
  *gfnt_bitmap_build_strike(build) = strike->strike;

  result = gfnt_ebdt_read_strike(face, strike, subtables, subtable_count, build,
      error);
  face->allocator->free_fn(face->allocator->ctx, subtables);
  gfnt_bitmap_build_finish(build, font, result == GFNT_OK);
  return result;
}

GFNT_Result gfnt_eblc_strike_glyphs(const GFNT_Face * face, size_t strike,
    const GFNT_BitmapFont ** out_font, GFNT_Error * error) {
  GFNT_Face * owner = (GFNT_Face *)face;
  const GFNT_Eblc * eblc = NULL;
  GFNT_EblcStrikeRequest request;
  GFNT_BitmapFont scratch;
  GFNT_Result result;

  if (!face || !out_font) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no face, or nowhere to put the strike");
  }
  result = gfnt_face_eblc(face, &eblc, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (strike >= eblc->strike_count) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG_EBLC, 0,
        GFNT_GLYPH_NONE, "a strike index the face does not have");
  }

  request.index = strike;
  result = gfnt_table_cached(face, &owner->eblc.strike_states[strike],
      &owner->eblc.strike_glyphs[strike], &scratch, sizeof scratch,
      gfnt_eblc_strike_parse, &request, gfnt_bitmap_release, error);
  if (result != GFNT_OK) {
    return result;
  }
  *out_font = &owner->eblc.strike_glyphs[strike];
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
