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
 * The `EBSC` table: one `BitmapScale` per size offered by scaling another.
 *
 * documentation/design.md section 7.5. `ebsc.h` says why this library reads the
 * records and scales nothing.
 *
 * Reference: OpenType Specification 1.9, "EBSC - Embedded Bitmap Scaling Table".
 */

#include <ghoti.io/font/macros.h>
#include <string.h>
#include "../tables/tables.h"
#include "ebsc.h"

/** Parse one `BitmapScale` at the reader's cursor. */
static GFNT_Result gfnt_ebsc_read_scale(const GFNT_Eblc * eblc,
    GFNT_Reader * reader, GFNT_EbscScale * out, GFNT_Error * error) {
  size_t offset = gfnt_reader_tell(reader);
  bool horizontal;

  if (gfnt_eblc_read_line_metrics(reader, &out->horizontal) != GFNT_OK
      || gfnt_eblc_read_line_metrics(reader, &out->vertical) != GFNT_OK
      || gfnt_read_u8(reader, &out->ppem_x) != GFNT_OK
      || gfnt_read_u8(reader, &out->ppem_y) != GFNT_OK
      || gfnt_read_u8(reader, &out->substitute_ppem_x) != GFNT_OK
      || gfnt_read_u8(reader, &out->substitute_ppem_y) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBSC, offset,
        GFNT_GLYPH_NONE, "an EBSC BitmapScale ends past the table");
  }

  if (out->ppem_x == 0 || out->ppem_y == 0) {
    // The same refusal gfnt_eblc_read_strike() makes, for the same reason: a
    // size of zero describes nothing, and it would sit in the list as a
    // candidate for any size search that ever consults these.
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBSC, offset,
        GFNT_GLYPH_NONE, "an EBSC record offers a pixel size of zero");
  }

  // The substitute, resolved to a strike index **here** rather than left to a
  // caller. Doing it once is why the struct carries an index at all: the table
  // states a ppem pair, every caller would otherwise search the strike list for
  // it, and a record naming a pair no strike has is a record that contradicts
  // itself - which this library can say and a caller receiving a bare pair
  // cannot.
  out->substitute_index = eblc->strike_count;
  for (size_t i = 0; i < eblc->strike_count; ++i) {
    if (eblc->strikes[i].strike.ppem_x == out->substitute_ppem_x
        && eblc->strikes[i].strike.ppem_y == out->substitute_ppem_y) {
      out->substitute_index = i;
      break;
    }
  }
  if (out->substitute_index == eblc->strike_count) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBSC, offset,
        GFNT_GLYPH_NONE, "an EBSC record defers to a pixel size no EBLC strike "
        "has");
  }
  // A record that substitutes *itself* is a loop with nothing at the end of it:
  // the size it offers is the size it says to make it from, so a caller
  // following the record arrives where it started. Nothing in the population
  // does this, and the alternative to refusing it is publishing a scaled size
  // that resolves to a strike a caller already had.
  if (out->ppem_x == out->substitute_ppem_x
      && out->ppem_y == out->substitute_ppem_y) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBSC, offset,
        GFNT_GLYPH_NONE, "an EBSC record defers to its own pixel size");
  }

  // Which of the record's two sbitLineMetrics a caller sees. See GFNT_EbscScale:
  // a BitmapScale states no flags, so the direction is the substitute strike's -
  // the only statement of it anywhere near these bytes.
  horizontal = (eblc->strikes[out->substitute_index].flags
      & GFNT_EBLC_HORIZONTAL) != 0;
  out->ascent = horizontal ? out->horizontal.ascender : out->vertical.ascender;
  // Through gfnt_eblc_descent_of(), not the raw byte: a BitmapScale's
  // sbitLineMetrics is the same record with the same unstated sign, and Anonymous
  // Pro writes a positive descender here exactly as it does in its strikes. One
  // rule for both, or the two answers for the same font disagree.
  out->descent = gfnt_eblc_descent_of(horizontal ? &out->horizontal
                                                 : &out->vertical);
  return GFNT_OK;
}

GFNT_Result gfnt_ebsc_parse(const GFNT_Face * face, void * out, void * context,
    GFNT_Error * error) {
  // One of this table per face, so there is nothing to select; the parameter is
  // here because every memo parser shares one signature.
  (void)context;
  GFNT_Ebsc * ebsc = out;
  const GFNT_Eblc * eblc = NULL;
  GFNT_Reader reader;
  GFNT_Result result;
  uint32_t count = 0;

  // gfnt_table_cached() copies this scratch into the memo whatever comes back,
  // so every failure below has to leave something the face can free (tables.h).
  memset(ebsc, 0, sizeof *ebsc);

  // The strike list first, because every record here is resolved against it. A
  // refused EBLC is therefore a refused EBSC, which is the honest answer: a
  // scaled size that defers to a strike list nobody can read is not a size.
  result = gfnt_face_eblc(face, &eblc, error);
  if (result != GFNT_OK) {
    return result;
  }

  result = gfnt_face_table_reader(face, GFNT_TAG_EBSC, &reader, error);
  if (result != GFNT_OK) {
    return result;
  }
  if (gfnt_read_u32(&reader, &ebsc->version) != GFNT_OK
      || gfnt_read_u32(&reader, &count) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_CORRUPT, GFNT_TAG_EBSC, 0,
        GFNT_GLYPH_NONE, "an EBSC shorter than its own header");
  }
  if (ebsc->version != GFNT_EBLC_VERSION) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, GFNT_TAG_EBSC, 0,
        GFNT_GLYPH_NONE, "an EBSC version this library does not read");
  }
  // The same cap as the strike list's, because these are sizes in the same sense
  // and a caller that bounded one meant to bound both. Wine's system.ttf states
  // 21 of them against two real strikes, so the ratio is not small.
  if (count > face->limits.max_strikes) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, GFNT_TAG_EBSC, 4,
        GFNT_GLYPH_NONE, "more EBSC records than GFNT_Limits::max_strikes");
  }
  if (count == 0) {
    // Well formed, and says the face offers no scaled sizes.
    return GFNT_OK;
  }
  // No bulk length check, for the reason gfnt_eblc_parse() states: it would
  // refuse exactly the inputs the per-record read refuses, leaving that read's
  // failure arm unreachable. `max_strikes` is what bounds the allocation.
  ebsc->scales = face->allocator->calloc_fn(face->allocator->ctx, count,
      sizeof *ebsc->scales);
  if (!ebsc->scales) {
    return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_EBSC, 0,
        GFNT_GLYPH_NONE, "no memory for the EBSC scale list");
  }
  ebsc->scale_count = count;

  for (uint32_t i = 0; i < count; ++i) {
    result = gfnt_ebsc_read_scale(eblc, &reader, &ebsc->scales[i], error);
    if (result != GFNT_OK) {
      return result;
    }
  }
  return GFNT_OK;
}

void gfnt_ebsc_release(const GFNT_Allocator * allocator, void * table) {
  GFNT_Ebsc * ebsc = table;

  if (!allocator || !ebsc) {
    return;
  }
  allocator->free_fn(allocator->ctx, ebsc->scales);
  ebsc->scales = NULL;
  ebsc->scale_count = 0;
}

bool gfnt_face_has_ebsc(const GFNT_Face * face) {
  if (!face) {
    return false;
  }
  // All three tables: a scale record names a strike, so an EBSC beside no strike
  // list states nothing a caller can use. See ebsc.h.
  return gfnt_face_has_table(face, GFNT_TAG_EBSC) && gfnt_face_has_eblc(face);
}

GFNT_Result gfnt_face_ebsc(const GFNT_Face * face, const GFNT_Ebsc ** out_ebsc,
    GFNT_Error * error) {
  GFNT_Ebsc scratch;
  GFNT_Result result;

  if (!face || !out_ebsc) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "no face, or nowhere to put the table");
  }
  if (!gfnt_face_has_ebsc(face)) {
    return gfnt_error_set(error, GFNT_ERR_UNSUPPORTED, 0, 0, GFNT_GLYPH_NONE,
        "the face has no EBSC with an EBLC and EBDT to go with it");
  }
  result = gfnt_table_cached(face, &((GFNT_Face *)face)->ebsc_state,
      &((GFNT_Face *)face)->ebsc, &scratch, sizeof scratch, gfnt_ebsc_parse,
      NULL, gfnt_ebsc_release, error);
  if (result != GFNT_OK) {
    return result;
  }
  *out_ebsc = &face->ebsc;
  return GFNT_OK;
}
