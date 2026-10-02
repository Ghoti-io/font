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
 * `EBSC`: sizes a face offers by **scaling** another size's bitmaps.
 *
 * documentation/design.md section 7.5. The table is a header and one 28-byte
 * `BitmapScale` per entry: two `sbitLineMetrics`, the ppem being offered, and the
 * ppem of the `EBLC` strike whose glyphs are to be scaled to make it.
 *
 * **This library reads the table and does not scale anything**, and that is the
 * whole design decision, so it is stated here rather than inferred from what is
 * missing:
 *
 *   * The specification says which strike substitutes and **not how**. There is no
 *     filter, no rounding rule and no statement about what becomes of a bearing,
 *     so any pixels this library produced would be its own invention presented as
 *     the font's.
 *   * **FreeType does not read `EBSC` at all**, and fontTools parses the records
 *     without scaling them. So there is no second reader of a scaled pixel
 *     anywhere - not a gap in this library's gates but a gap in the world - and
 *     §14 does not permit a format to be implemented with nothing able to
 *     contradict it.
 *   * What a caller actually needs is decidable: *this face offers 8 ppem, and
 *     says to make it from the 10 ppem strike*. That is in the table, it is
 *     checkable against fontTools, and a caller that wants to scale can - with its
 *     own filter, which is a rendering decision and not a font-reading one.
 *
 * So ::gfnt_face_scaled_strike_count() and ::gfnt_face_scaled_strike_at() report
 * what the table states, and ::GFNT_Strike never comes from here.
 * ::gfnt_face_select_strike() does not consult it either: that function's contract
 * is "a strike, or the outlines", and a third answer would change what every
 * existing caller is told. §7.5 records that as the open question it is.
 *
 * **The substitute is resolved to a strike index here**, because the table names a
 * ppem pair and a caller should not have to search the strike list for it - and
 * because a record naming a substitute no strike has is a record that contradicts
 * itself, which is a refusal this library can make and a caller cannot.
 *
 * Reference: OpenType Specification 1.9, "EBSC - Embedded Bitmap Scaling Table".
 */

#ifndef GHOTI_IO_GFNT_EBSC_H
#define GHOTI_IO_GFNT_EBSC_H

#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/macros.h>
#include "eblc.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The tag this file reads: `EBSC`. */
#define GFNT_TAG_EBSC GFNT_TAG('E', 'B', 'S', 'C')

/**
 * @brief Bytes in one `BitmapScale` record on disk.
 *
 * Twenty-eight: two twelve-byte `sbitLineMetrics` and four `uint8`. Spelled as a
 * constant rather than `sizeof` a struct, because ::GFNT_EbscScale keeps only what
 * is read.
 */
#define GFNT_EBSC_SCALE_BYTES 28u

/** @brief Bytes in the `EBSC` header: a version and a count. */
#define GFNT_EBSC_HEADER_BYTES 8u

/**
 * One `BitmapScale`: a size offered, and the strike it defers to.
 *
 * `substitute_index` is this library's, not the table's: the record states a ppem
 * pair and the resolution to a strike is done once, at parse time, so that every
 * caller gets the same answer and a record naming a substitute that does not exist
 * is refused rather than left for a caller to trip over.
 */
typedef struct GFNT_EbscScale {
  uint8_t ppem_x;             ///< The horizontal size this record offers.
  uint8_t ppem_y;             ///< The vertical size this record offers.
  uint8_t substitute_ppem_x;  ///< The strike's horizontal size to scale from.
  uint8_t substitute_ppem_y;  ///< The strike's vertical size to scale from.
  size_t substitute_index;    ///< Which `EBLC` strike that is.
  GFNT_EblcLineMetrics horizontal;  ///< The record's own, not the strike's.
  GFNT_EblcLineMetrics vertical;
  /**
   * Which of those two the caller sees, resolved at parse time.
   *
   * **A `BitmapScale` states no `flags`**, where a `bitmapSizeTable` does - so the
   * record carries two `sbitLineMetrics` and nothing in it says which is the one
   * to use. The direction therefore comes from the **substitute strike**, whose
   * flags are the only statement of it anywhere near these bytes and whose pixels
   * the record defers to anyway.
   *
   * It is resolved here rather than in the accessor because this is where the
   * substitute strike is already in hand; the first draft of the accessor picked a
   * direction by testing whether the horizontal metrics were non-zero, which is
   * not a rule the format has and would have answered "vertical" for a horizontal
   * strike whose ascent and descent are both legitimately zero.
   */
  int8_t ascent;
  int8_t descent;
} GFNT_EbscScale;

/**
 * A parsed `EBSC`: its version and its scale records.
 *
 * `scales` is allocated on the face's allocator and freed by
 * ::gfnt_ebsc_release(). Nothing here points into the blob.
 */
typedef struct GFNT_Ebsc {
  uint32_t version;         ///< 0x00020000 in every file seen.
  GFNT_EbscScale * scales;  ///< One per `BitmapScale`, in table order.
  size_t scale_count;
} GFNT_Ebsc;

/** The `EBSC` parser, for ::gfnt_table_cached(). */
GFNT_Result gfnt_ebsc_parse(const GFNT_Face * face, void * out, void * context,
    GFNT_Error * error);

/** Release what a parse allocated. Harmless on a zeroed struct. */
void gfnt_ebsc_release(const GFNT_Allocator * allocator, void * table);

/**
 * The face's parsed `EBSC`, parsing it on first use.
 *
 * @param face The face.
 * @param out_ebsc Receives it. Borrowed; it lives as long as the face.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_UNSUPPORTED for a face with no `EBSC`, or one
 *   whose `EBLC` this library will not read - a scaled size that defers to a
 *   strike list nobody can see is not an answer; or what the parse returned.
 */
GFNT_Result gfnt_face_ebsc(const GFNT_Face * face, const GFNT_Ebsc ** out_ebsc,
    GFNT_Error * error);

/**
 * Whether this face has an `EBSC` **and** the `EBLC`/`EBDT` it defers to.
 *
 * All three, for the reason ::gfnt_face_has_eblc() wants two: a scale record
 * names a strike, so an `EBSC` beside no strike list states nothing a caller can
 * use. A face like that reports no scaled sizes rather than a list of sizes that
 * resolve to nothing.
 */
bool gfnt_face_has_ebsc(const GFNT_Face * face);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_EBSC_H
