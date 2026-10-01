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
 * `EBLC`: which sizes an sfnt carries bitmaps at.
 *
 * documentation/design.md section 7.5. This is the *list*, not the pixels: the
 * table's header and one `bitmapSizeTable` per strike, which is what
 * ::gfnt_face_strike_count() and ::gfnt_face_strike_at() answer from. `EBDT` is
 * where the glyphs are and is read separately, per strike, on demand.
 *
 * **The split is not tidiness.** `uming.ttc` - four of the six faces in Debian
 * that have this table at all - carries six strikes of 27,123 glyphs with 2,305
 * index subtables each. Parsing those to open a face would cost 55,000 subtable
 * headers before a caller asked for anything, and section 5.3 promises that
 * opening a 30 MB collection to ask for one glyph costs one glyph. So the list
 * is one memo on the face and a strike's index is parsed when a glyph from that
 * strike is first wanted.
 *
 * Only `EBLC` is read. Apple's `bloc` has the same layout and `CBLC` has the
 * same layout with PNG payloads behind it, and both still report
 * ::GFNT_ERR_UNSUPPORTED - not because the list would be hard but because a
 * strike list a caller cannot then get glyphs from is a worse answer than an
 * honest refusal, and neither has a fixture yet.
 *
 * Reference: OpenType Specification 1.9, "EBLC - Embedded Bitmap Location
 * Table"; the Apple TrueType Reference Manual, "bloc".
 */

#ifndef GHOTI_IO_GFNT_EBLC_H
#define GHOTI_IO_GFNT_EBLC_H

#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/macros.h>
#include "../reader/reader.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The tag this file reads: `EBLC`. */
#define GFNT_TAG_EBLC GFNT_TAG('E', 'B', 'L', 'C')
/** @brief Where its glyphs are: `EBDT`. */
#define GFNT_TAG_EBDT GFNT_TAG('E', 'B', 'D', 'T')

/**
 * One `sbitLineMetrics`: how a line of this strike is spaced.
 *
 * Both directions are stored because the table states both and a vertical
 * run needs the second, even though nothing reads it yet. The ascent and
 * descent a caller sees are ::GFNT_Strike's, which take the direction the
 * strike's `flags` say it is for.
 */
typedef struct GFNT_EblcLineMetrics {
  int8_t ascender;          ///< Pixels above the baseline, positive.
  int8_t descender;         ///< Pixels below it, **negative** (M18).
  uint8_t width_max;        ///< The widest glyph's advance, pixels.
  int8_t caret_slope_numerator;
  int8_t caret_slope_denominator;
  int8_t caret_offset;
  int8_t min_origin_sb;     ///< Smallest origin-to-ink side bearing.
  int8_t min_advance_sb;    ///< Smallest ink-to-advance side bearing.
  int8_t max_before_bl;     ///< Largest ink extent above the baseline.
  int8_t min_after_bl;      ///< Largest ink extent below it, negative.
} GFNT_EblcLineMetrics;

/**
 * One strike: the public ::GFNT_Strike, plus where to find its glyph index.
 *
 * `index_array_offset` and `index_tables_size` are offsets *into the `EBLC`
 * table*, which is what every offset in this table is relative to - unlike
 * `EBDT`'s, which are relative to that table. Keeping the two straight is the
 * one bookkeeping error this format invites.
 */
typedef struct GFNT_EblcStrike {
  GFNT_Strike strike;             ///< ppem, depth, kind and the baseline.
  uint32_t index_array_offset;    ///< `indexSubTableArrayOffset`, from `EBLC`.
  uint32_t index_tables_size;     ///< `indexTablesSize`, **as stated** - see below.
  uint32_t index_subtable_count;  ///< `numberOfIndexSubTables`.
  uint32_t colour_ref;            ///< `colorRef`, unused by the specification.
  uint16_t start_glyph;           ///< `startGlyphIndex`, inclusive.
  uint16_t end_glyph;             ///< `endGlyphIndex`, inclusive.
  uint8_t bit_depth;              ///< 1, 2, 4, 8, or 32 for colour.
  int8_t flags;                   ///< 1 horizontal, 2 vertical, per the spec.
  GFNT_EblcLineMetrics horizontal;
  GFNT_EblcLineMetrics vertical;
} GFNT_EblcStrike;

/**
 * A parsed `EBLC`: its version and its strikes.
 *
 * `strikes` is allocated on the face's allocator and freed by
 * ::gfnt_eblc_release(). Nothing here points into the blob, so a strike survives
 * being copied out.
 */
typedef struct GFNT_Eblc {
  uint32_t version;            ///< 0x00020000 in every file seen.
  GFNT_EblcStrike * strikes;   ///< One per `bitmapSizeTable`, in table order.
  size_t strike_count;
} GFNT_Eblc;

/** The `EBLC` parser, for ::gfnt_table_cached(). */
GFNT_Result gfnt_eblc_parse(const GFNT_Face * face, void * out,
    GFNT_Error * error);

/** Release what a parse allocated. Harmless on a zeroed struct. */
void gfnt_eblc_release(const GFNT_Allocator * allocator, GFNT_Eblc * eblc);

/**
 * The face's parsed `EBLC`, parsing it on first use.
 *
 * @param face The face.
 * @param out_eblc Receives it. Borrowed; it lives as long as the face.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_UNSUPPORTED for a face with no `EBLC`; or what
 *   the parse returned.
 */
GFNT_Result gfnt_face_eblc(const GFNT_Face * face, const GFNT_Eblc ** out_eblc,
    GFNT_Error * error);

/**
 * Whether this face has an `EBLC` **and** the `EBDT` its strikes index into.
 *
 * Both, because neither names a glyph without the other - the same rule
 * ::gfnt_sfnt_producer() applies to `glyf` and `loca`. A face with one of them
 * has no strikes this library can list, and saying "six strikes" about a font
 * whose pixels are not in the file would be a worse answer than none.
 */
bool gfnt_face_has_eblc(const GFNT_Face * face);

/** Write an `EBLC` as text, one line per strike, for the differentials. */
GFNT_Result gfnt_eblc_dump(const GFNT_Eblc * eblc, FILE * out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_EBLC_H
