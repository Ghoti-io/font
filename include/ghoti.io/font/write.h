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
 * Writing fonts: the sfnt and WOFF 1 serialisers, and the subsetter.
 *
 * Tier W (documentation/design.md sections 4 and 12). It reads through tier 0
 * and nothing above it, and it is not part of the `font.h` umbrella: a consumer
 * that only reads should not link a writer.
 *
 * Every blob this header produces is owned by the caller and released with
 * ::gfnt_blob_destroy(); the face that reads it back is loaded the ordinary way.
 */

#ifndef GHOTI_IO_GFNT_WRITE_H
#define GHOTI_IO_GFNT_WRITE_H

#include <stddef.h>
#include <stdint.h>
#include <ghoti.io/font/allocator.h>
#include <ghoti.io/font/blob.h>
#include <ghoti.io/font/core.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/font/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * One table to be written: its tag and its bytes.
 *
 * The bytes are borrowed for the duration of the call. Their length need not be
 * a multiple of four - the serialiser pads, and the directory states the
 * unpadded length.
 */
typedef struct GFNT_WriteTable {
  GFNT_Tag tag;         ///< The table's tag.
  const uint8_t * data; ///< Its bytes; may be NULL only when @c length is zero.
  size_t length;        ///< How many.
} GFNT_WriteTable;

/**
 * @brief Serialise tables as an sfnt.
 *
 * Tables are written sorted by tag, each starting on a four-byte boundary and
 * padded with zeros, under an offset table whose `searchRange`,
 * `entrySelector` and `rangeShift` are derived from the count. Each directory
 * checksum is computed from the bytes, and when a `head` table of at least
 * twelve bytes is among them its `checkSumAdjustment` is computed last, over
 * the whole file, as the specification requires; whatever the caller put in
 * those four bytes is replaced.
 *
 * Nothing else about a table is looked at: this is a serialiser, not a
 * validator, and a font that is wrong going in is wrong coming out.
 *
 * @param flavour The sfnt version: `0x00010000` for TrueType outlines, `OTTO`
 *   for CFF.
 * @param tables The tables, in any order.
 * @param count How many. At least one, and no more than `limits->max_tables`.
 * @param limits Caps, or NULL for the defaults. The finished font may not exceed
 *   `max_blob_bytes`.
 * @param allocator Where the blob comes from, or NULL for the default.
 * @param out_blob Receives the font. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID for no tables, a repeated tag or a
 *   table with bytes missing; ::GFNT_ERR_LIMIT; ::GFNT_ERR_OOM.
 */
GFNT_API GFNT_Result gfnt_write_sfnt(GFNT_Tag flavour,
    const GFNT_WriteTable * tables, size_t count, const GFNT_Limits * limits,
    const GFNT_Allocator * allocator, GFNT_Blob ** out_blob, GFNT_Error * error);

/**
 * @brief Serialise tables as a WOFF 1 file.
 *
 * The same sfnt ::gfnt_write_sfnt() builds, with each table compressed on its
 * own (zlib, through `compress`) and stored uncompressed whenever compression
 * does not make it smaller, as the format requires. No metadata or private
 * block is written. The font version in the header is `head.fontRevision`'s
 * integer and fraction halves, or zero without a `head`.
 *
 * @param flavour As for ::gfnt_write_sfnt().
 * @param tables As for ::gfnt_write_sfnt().
 * @param count As for ::gfnt_write_sfnt().
 * @param limits As for ::gfnt_write_sfnt().
 * @param allocator As for ::gfnt_write_sfnt().
 * @param out_blob Receives the WOFF file. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return As for ::gfnt_write_sfnt(), and ::GFNT_ERR_INTERNAL if the
 *   compression library fails for a reason other than memory.
 */
GFNT_API GFNT_Result gfnt_write_woff(GFNT_Tag flavour,
    const GFNT_WriteTable * tables, size_t count, const GFNT_Limits * limits,
    const GFNT_Allocator * allocator, GFNT_Blob ** out_blob, GFNT_Error * error);

/**
 * Which source glyph each glyph of a subset is.
 *
 * Filled by ::gfnt_subset() when ::GFNT_SubsetOptions::map points at one; release
 * it with ::gfnt_subset_map_free(). Entry @c i is the id in the source face of
 * the subset's glyph @c i, or 0xFFFFFFFF for a glyph of a retained-id subset that
 * was dropped and is empty. A caller who writes a PDF `ToUnicode` or a `W` array
 * needs this; so does anyone checking a renumbered subset against its source.
 */
typedef struct GFNT_SubsetMap {
  uint32_t * old_of_new; ///< One entry per glyph of the subset.
  size_t count;          ///< How many.
} GFNT_SubsetMap;

/**
 * What to keep, and how.
 *
 * Initialise with ::gfnt_subset_options_init() and then set what is wanted:
 * a zeroed struct is a valid request for "glyph 0 only", but it is the defaults
 * that say the subsetter closes over `GSUB`, so a caller who builds the struct
 * by hand gets the opposite of what the design asks for.
 */
typedef struct GFNT_SubsetOptions {
  const uint32_t * codepoints; ///< Characters to keep, in any order; repeats are fine.
  size_t codepoint_count;      ///< How many.
  const uint32_t * glyphs;     ///< Glyph ids to keep as well, for glyphs no character reaches.
  size_t glyph_count;          ///< How many.
  /**
   * Keep every surviving glyph at its own id, and empty the ones that were dropped.
   *
   * What a PDF `CIDFontType2` with an identity `CIDToGIDMap` needs. The layout
   * tables are copied as they are in this mode and rewritten otherwise.
   */
  bool retain_gids;
  /** Do not add the glyphs `GSUB` can turn the kept ones into. Off by default. */
  bool no_gsub_closure;
  /** Drop `fpgm`, `prep`, `cvt ` and every glyph's instructions. */
  bool drop_hinting;
  /**
   * Leave out `GSUB`, `GPOS`, `GDEF`, `kern`, `vhea`, `vmtx` and `VORG`.
   *
   * By default they are carried: copied as they are when ::retain_gids keeps the
   * ids where they were, rewritten for the new numbering when it does not.
   */
  bool drop_layout;
  /**
   * Restrict `GSUB` to lookups reached from these features.
   *
   * The closure follows only those, and a renumbered `GSUB` keeps only those
   * (the other features stay listed, with no lookups, so which features exist does
   * not change). NULL, or a count of zero, means every feature the table lists.
   * `GPOS` is not restricted.
   */
  const GFNT_Tag * features;
  size_t feature_count;        ///< How many.
  /**
   * Where to put the glyph numbering, or NULL if the caller does not want it.
   * Written only on success; the caller releases it with ::gfnt_subset_map_free().
   */
  GFNT_SubsetMap * map;
} GFNT_SubsetOptions;

/**
 * @brief Release a map ::gfnt_subset() filled in.
 *
 * @param map The map. NULL, and a map with nothing in it, are ignored.
 * @param allocator The allocator that was passed to ::gfnt_subset(), or NULL.
 */
GFNT_API void gfnt_subset_map_free(GFNT_SubsetMap * map, const GFNT_Allocator * allocator);

/**
 * @brief Set an options struct to the defaults: nothing requested, `GSUB` closed over.
 *
 * @param options The struct. NULL is ignored.
 */
GFNT_API void gfnt_subset_options_init(GFNT_SubsetOptions * options);

/**
 * @brief Write a font holding only some of a face's glyphs.
 *
 * The glyphs kept are glyph 0, the glyphs the requested codepoints map to
 * through the face's preferred `cmap` subtable, and the requested glyph ids,
 * closed over composite components always and over `GSUB` unless
 * ::GFNT_SubsetOptions::no_gsub_closure. The closure over contextual lookups is
 * an over-approximation (it may keep a glyph no text can reach, and never drops
 * one it can).
 *
 * **Request what the shaper borrows.** Some characters are shaped with another's
 * glyph: U+0020 stands in for an invisible joiner or a fixed-width space, `0` and
 * `.` size the figure and punctuation spaces, U+2010 replaces a non-breaking
 * hyphen, and U+25CC is the dotted circle of a broken Indic cluster. The closure
 * has no way to know the text will reach them. The same holds for composition:
 * the shaper composes a base with any later mark, so the precomposed characters
 * of the text's pairs have to be asked for too.
 *
 * The output is a TrueType-outline sfnt of `head`, `hhea`, `maxp`, `hmtx`,
 * `cmap` (formats 4 and 12 for the requested codepoints, in the Unicode and
 * Windows records), `loca`, `glyf`, `post` (format 3, no glyph names), and, when
 * the source has them, `OS/2` (first and last character updated), `name`,
 * `gasp`, and the hinting tables. The layout tables (`GSUB`, `GPOS`, `GDEF`), the
 * legacy `kern` table and the vertical metrics (`vhea`, `vmtx`, `VORG`) are
 * carried unless ::GFNT_SubsetOptions::drop_layout is set. With
 * ::GFNT_SubsetOptions::retain_gids they are copied untouched: glyph ids have not
 * moved, so every reference in them is still valid, and a glyph that was dropped
 * is an empty outline. Without it they are **rewritten** for the new numbering:
 * each lookup the kept glyphs can call is rebuilt with only the entries and rules
 * that name kept glyphs (a rule survives exactly when the `GSUB` closure counted
 * it), lookups nothing calls are left out, pair and mark-attachment classes no
 * kept glyph uses are removed, and `GDEF` keeps its classes, attachment points,
 * ligature carets and mark sets for the kept glyphs. `kern` is carried as format 0
 * subtables only and is left out whole if it has any other form. A subtable the
 * rewrite would have to split to keep 16-bit offsets is ::GFNT_ERR_LIMIT, and a
 * layout table with `FeatureVariations` is ::GFNT_ERR_UNSUPPORTED (set
 * ::GFNT_SubsetOptions::drop_layout to subset such a font anyway).
 * Anything else in the source is not written.
 *
 * **Not built**: `CFF`, `CFF2` and Type 1 outlines, variable fonts (`fvar`,
 * `gvar`), bitmap and colour tables, the Apple tables (`morx`, `mort`, `kerx`,
 * `trak`, `ankr`), class-based `kern`, `cmap` format 14, and the family rename
 * the licence reading in documentation/design.md section 14.5 wants. Each is
 * refused or dropped as stated here, not approximated.
 *
 * @param face The source face.
 * @param options What to keep. NULL means the defaults with nothing requested.
 * @param limits Caps, or NULL for the defaults.
 * @param allocator Where the output comes from, or NULL for the default.
 * @param out_blob Receives the font. Written only on success.
 * @param error Receives a diagnostic on failure, or NULL.
 * @return ::GFNT_OK; ::GFNT_ERR_INVALID for a glyph id the face does not have;
 *   ::GFNT_ERR_UNSUPPORTED for a face this cannot subset (see above);
 *   ::GFNT_ERR_CORRUPT for source tables that contradict themselves, including
 *   a `GSUB` that cannot be walked (set ::GFNT_SubsetOptions::no_gsub_closure to
 *   subset such a font anyway); ::GFNT_ERR_LIMIT; ::GFNT_ERR_OOM.
 */
GFNT_API GFNT_Result gfnt_subset(const GFNT_Face * face,
    const GFNT_SubsetOptions * options, const GFNT_Limits * limits,
    const GFNT_Allocator * allocator, GFNT_Blob ** out_blob, GFNT_Error * error);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GFNT_WRITE_H
