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
#include "bitmap.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The tag this file reads: `EBLC`. */
#define GFNT_TAG_EBLC GFNT_TAG('E', 'B', 'L', 'C')
/** @brief Where its glyphs are: `EBDT`. */
#define GFNT_TAG_EBDT GFNT_TAG('E', 'B', 'D', 'T')

/**
 * @brief Bytes in a `BigGlyphMetrics`, which index formats 2 and 5 carry.
 *
 * Eight: the horizontal five and a vertical three. ::GFNT_EblcMetrics keeps only
 * the five, so this is the size on disk rather than `sizeof` anything.
 */
#define GFNT_EBLC_METRICS_BYTES 8u

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
 * One glyph's box and advance, as `EBLC` or `EBDT` states it.
 *
 * The horizontal three only. `BigGlyphMetrics` carries a vertical set as well and
 * nothing reads it: no accessor in this library asks for a vertical advance yet,
 * and a field parsed into a struct nobody reads is the shape that makes a
 * differential compare a number neither side means.
 *
 * `SmallGlyphMetrics` is these five values and `BigGlyphMetrics` is these five
 * plus three, so one struct serves both and the image format decides how many
 * bytes were read.
 */
typedef struct GFNT_EblcMetrics {
  uint8_t height;     ///< Rows.
  uint8_t width;      ///< Pixels across.
  int8_t bearing_x;   ///< Left side bearing; may be negative.
  int8_t bearing_y;   ///< Top row's edge above the baseline, y-up.
  uint8_t advance;    ///< Pen movement, pixels.
} GFNT_EblcMetrics;

/**
 * One index subtable: which glyphs it covers, and where to find them.
 *
 * `body_offset` is into the `EBLC` table and points past the eight-byte
 * `indexSubHeader`; `image_data_offset` is into **`EBDT`**. The two bases are the
 * one piece of bookkeeping this format reliably invites an error in, so they are
 * named for the table they belong to rather than both being "offset".
 *
 * `image_size` and `metrics` are the constant ones index formats 2 and 5 state for
 * every glyph they cover. `sparse_count` is the glyph-id array's length in formats
 * 4 and 5. A field a format does not have is zero, and nothing reads it.
 */
typedef struct GFNT_EblcSubtable {
  uint16_t first_glyph;       ///< `firstGlyphIndex`, inclusive.
  uint16_t last_glyph;        ///< `lastGlyphIndex`, inclusive.
  uint16_t index_format;      ///< 1-5.
  uint16_t image_format;      ///< 1, 2, 5, 6, 7, or 8 and 9, the composites.
  uint32_t image_data_offset; ///< Into `EBDT`.
  size_t body_offset;         ///< Into `EBLC`, past the `indexSubHeader`.
  uint32_t image_size;        ///< Index formats 2 and 5: bytes per glyph.
  GFNT_EblcMetrics metrics;   ///< Index formats 2 and 5: every glyph's box.
  uint32_t sparse_count;      ///< Index formats 4 and 5: glyph ids listed.
} GFNT_EblcSubtable;

/**
 * A parsed `EBLC`: its version and its strikes.
 *
 * `strikes` is allocated on the face's allocator and freed by
 * ::gfnt_eblc_release(). Nothing here points into the blob, so a strike survives
 * being copied out.
 */
/**
 * Declared rather than included: `GFNT_Cached` lives in `sfnt.h`, and `sfnt.h`
 * includes *this* file to put the memo on the face. A pointer to an incomplete
 * type is all this header needs, and `eblc.c` sees the definition through
 * `tables.h`.
 */
struct GFNT_Cached;

typedef struct GFNT_Eblc {
  uint32_t version;            ///< 0x00020000 in every file seen.
  GFNT_EblcStrike * strikes;   ///< One per `bitmapSizeTable`, in table order.
  size_t strike_count;
  /**
   * One memo per strike, for the glyphs - allocated with `strikes`.
   *
   * A strike's glyph data is parsed on first use and each strike separately, so
   * each needs its own `done` flag and its own published copy. They live here
   * rather than on the face because their number is the table's to state: a face
   * cannot declare an array of them before anything has read `numSizes`.
   */
  struct GFNT_Cached * strike_states;
  GFNT_BitmapFont * strike_glyphs;
} GFNT_Eblc;

/** The `EBLC` parser, for ::gfnt_table_cached(). */
GFNT_Result gfnt_eblc_parse(const GFNT_Face * face, void * out,
    void * context,
    GFNT_Error * error);

/**
 * Release what a parse allocated. Harmless on a zeroed struct.
 *
 * `void *` rather than `GFNT_Eblc *` so that it **is**
 * ::gfnt_table_cached()'s release hook rather than needing a wrapper around it -
 * the same reason ::gfnt_eblc_parse() takes one. A wrapper would be a function
 * nothing could reach: the race it exists for happens only for a memo parsed
 * lazily, so for the memos parsed during the face load the wrapper would be
 * uncovered and uncoverable.
 */
void gfnt_eblc_release(const GFNT_Allocator * allocator, void * table);

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

/**
 * One strike's glyphs, parsed on first use, as a ::GFNT_BitmapFont.
 *
 * **One memo per strike**, which is the whole reason ::gfnt_table_cached() takes a
 * context. A strike is parsed whole - every glyph it covers, pixels and all, into
 * one arena - because ::GFNT_BitmapGlyph::bits is borrowed from the face and has
 * to outlive the call, so the face must own it; and it is parsed *per strike*
 * because `uming.ttc` has six of 27,123 glyphs and nothing should read five of
 * them to answer about the sixth.
 *
 * The cost is still a whole strike to answer one glyph. That is the honest
 * statement of it: an arena grown glyph by glyph under the face's lock would be
 * the smaller answer, and is not worth its complexity until something measures
 * this as a problem.
 *
 * The returned font's records are indexed by the **face's** glyph id, not by a
 * position in the strike: a strike is sparse over the face, so a glyph it does not
 * cover is a record with ::GFNT_BitmapRecord::present false - which is a different
 * fact from a glyph with no pixels, and the reason that field exists.
 *
 * @param face The face.
 * @param strike Which strike, from 0.
 * @param out_font Receives it. Borrowed; it lives as long as the face.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID for a strike the face does not have;
 *   ::GFNT_ERR_UNSUPPORTED for a face with no `EBLC`; or what the parse returned.
 */
GFNT_Result gfnt_eblc_strike_glyphs(const GFNT_Face * face, size_t strike,
    const GFNT_BitmapFont ** out_font, GFNT_Error * error);

/**
 * Whether an index format stores its glyph sizes as differences between offsets.
 *
 * Formats 1, 3 and 4 do; formats 2 and 5 state one constant size instead. The
 * distinction decides what a size of **zero** means, which is why it is a named
 * predicate rather than a condition written out at the point of use: an offset
 * array giving a glyph no bytes is the format saying this strike has no bitmap
 * for that glyph, while a constant size of zero is a statement about every glyph
 * the subtable covers. `ebdt.c` had the first rule wrong and reported 15,570 of
 * Konatu.ttf's glyphs as carried where two other readers say 2,323.
 *
 * Together with ::gfnt_eblc_index_is_constant these partition the five index
 * formats, and `gfnt_eblc_index_subtable_known()` is what keeps a sixth from
 * reaching either.
 */
static inline bool gfnt_eblc_index_has_offsets(uint16_t index_format) {
  return index_format == 1 || index_format == 3 || index_format == 4;
}

/** Whether an index format states a constant size and metrics. */
static inline bool gfnt_eblc_index_is_constant(uint16_t index_format) {
  return index_format == 2 || index_format == 5;
}

/**
 * Parse one strike's index subtable array into @p out.
 *
 * Split out of the glyph parse so that a test can read the subtable headers of a
 * strike whose glyph data is deliberately broken, and so that `ebdt.c` has one
 * place to look up which subtable covers a glyph.
 *
 * @param out Receives `subtable_count` entries, allocated on the face's allocator.
 */
GFNT_Result gfnt_eblc_strike_index(const GFNT_Face * face,
    const GFNT_EblcStrike * strike, GFNT_EblcSubtable ** out_subtables,
    size_t * out_count, GFNT_Error * error);

/**
 * Decode every glyph of one strike into @p build.
 *
 * `ebdt.c`'s entry point. Walks the strike's index, and for each glyph the strike
 * covers reads its metrics and its rows in whichever of the image formats the
 * subtable names.
 */
GFNT_Result gfnt_ebdt_read_strike(const GFNT_Face * face,
    const GFNT_EblcStrike * strike, const GFNT_EblcSubtable * subtables,
    size_t subtable_count, GFNT_BitmapBuild * build, GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_EBLC_H
